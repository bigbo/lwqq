#include <curl/curl.h>
#include <string.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef WIN32
#undef SLIST_ENTRY
#endif

#if (defined WITH_LIBEV) && (defined __MINGW32__)
#define PIPE_SIZE 4096
#define pipe(fd) _pipe(fd, PIPE_SIZE, 0)
#endif

#include "async.h"
#include "smemory.h"
#include "http.h"
#include "logger.h"
#include "queue.h"
#include "utility.h"
#include "async_impl.h"
#include "internal.h"

//#define LWQQ_HTTP_USER_AGENT "Mozilla/5.0 (X11; Linux x86_64; rv:10.0)
// Gecko/20100101 Firefox/10.0"
#define LWQQ_HTTP_USER_AGENT                                                   \
   "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "   \
   "Chrome/33.0.1750.152 Safari/537.36"

static int lwqq_http_do_request(LwqqHttpRequest* request, int method,
                                char* body);
static void lwqq_http_set_header(LwqqHttpRequest* request, const char* name,
                                 const char* value);
static const char* lwqq_http_get_header(LwqqHttpRequest* request,
                                        const char* name);
static void lwqq_http_add_form(LwqqHttpRequest* request, LWQQ_FORM form,
                               const char* name, const char* value);
static void lwqq_http_add_file_content(LwqqHttpRequest* request,
                                       const char* name, const char* filename,
                                       const void* data, size_t size,
                                       const char* extension);
static LwqqAsyncEvent* lwqq_http_do_request_async(LwqqHttpRequest* request,
                                                  int method, char* body,
                                                  LwqqCommand);
static void delay_add_handle();

typedef struct GLOBAL {
   CURLM* multi;
   int still_running;
   int conn_length; //< make sure there are only cache_size http request
   // running
   int cache_size;
   TAILQ_HEAD(, D_ITEM) conn_link;
   LwqqAsyncTimerHandle timer_event;
   LwqqAsyncIoHandle add_listener;
   TAILQ_HEAD(, D_ITEM) add_link;
#ifdef WITH_LIBEV
   int pipe_fd[2];
#endif
} GLOBAL;

typedef enum {
   HTTP_UNEXPECTED_RECV = 1 << 0,
   HTTP_FORCE_CANCEL = 1 << 1,
   HTTP_SYNCED = 1 << 2
} HttpBits;

struct trunk_entry {
   char* trunk;
   size_t size;
   SIMPLEQ_ENTRY(trunk_entry) entries;
};

typedef struct LwqqHttpRequest_ {
   LwqqHttpRequest parent;
   char* cookie; // cookie used in current request
   HttpBits bits; // store http internal status
   short retry_;
   short timeout; // timeout orginal
   short tmo_inc; // timeout increment
#ifdef HAVE_OPEN_MEMSTREAM
   FILE* mem_buf;
#else
   SIMPLEQ_HEAD(, trunk_entry) trunks;
#endif
} LwqqHttpRequest_;

typedef struct LwqqHttpHandle_ {
   LwqqHttpHandle parent;
   CURLSH* share;
   pthread_mutex_t share_lock[4];
} LwqqHttpHandle_;

struct CookieExt {
   LwqqExtension super;
   const LwqqCommand* login_b;
   const LwqqCommand* login_c;
   const LwqqCommand* logout;
   const LwqqCommand* clean;
   char* cookie_file;
};

static TABLE_BEGIN(proxy_map, long, 0) TR(LWQQ_HTTP_PROXY_HTTP, CURLPROXY_HTTP)
    TR(LWQQ_HTTP_PROXY_SOCKS4, CURLPROXY_SOCKS4)
    TR(LWQQ_HTTP_PROXY_SOCKS5, CURLPROXY_SOCKS5) TABLE_END()

    static TABLE_BEGIN(errno_map, LwqqErrorCode, LWQQ_EC_ERROR)
    TR(CURLE_OK, LWQQ_EC_OK) TR(CURLE_OPERATION_TIMEDOUT, LWQQ_EC_TIMEOUT_OVER)
    TR(CURLE_ABORTED_BY_CALLBACK, LWQQ_EC_CANCELED)
    TR(CURLE_TOO_MANY_REDIRECTS, LWQQ_EC_OK)
    TR(CURLE_SSL_CONNECT_ERROR, LWQQ_EC_SSL_ERROR) TABLE_END()

    static GLOBAL global = { 0 };
static pthread_cond_t async_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ev_block_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t add_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct S_ITEM {
   /**@brief 全局事件循环*/
   curl_socket_t sockfd;
   int action;
   CURL* easy;
   /**@brief ev重用标志,一直为1 */
   int evset;
   LwqqAsyncIoHandle ev;
} S_ITEM;
typedef struct D_ITEM {
   LwqqCommand cmd;
   LwqqHttpRequest* req;
   LwqqAsyncEvent* event;
   // void* data;
   TAILQ_ENTRY(D_ITEM) entries;
} D_ITEM;
/* For async request */

#ifndef NDEBUG
int lwqq_gdb_whats_running()
{
   D_ITEM* item;
   char* url;
   int num = 0;
   TAILQ_FOREACH(item, &global.conn_link, entries)
   {
      curl_easy_getinfo(item->req->req, CURLINFO_EFFECTIVE_URL, &url);
      lwqq_puts(url);
      num++;
   }
   return num;
}
#endif

LwqqFeatures lwqq__http_check_feature()
{
   return (curl_version_info(CURLVERSION_NOW)->features
           & CURL_VERSION_ASYNCHDNS)
              ? LWQQ_WITH_ASYNCHDNS
              : 0;
}

#ifndef HAVE_OPEN_MEMSTREAM
static size_t write_content(const char* ptr, size_t size, size_t nmemb,
                            void* userdata)
{
   LwqqHttpRequest* req = (LwqqHttpRequest*)userdata;
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   long http_code = 0;
   size_t sz_ = size * nmemb;
   curl_easy_getinfo(req->req, CURLINFO_RESPONSE_CODE, &http_code);
   // this is a redirection. ignore it.
   if (http_code == 301 || http_code == 302) {
      return sz_;
   }
   char* position = NULL;
   double length = 0.0;
   curl_easy_getinfo(req->req, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &length);
   if (req->response == NULL && SIMPLEQ_EMPTY(&req_->trunks)) {
      if (length != -1.0 && length != 0.0) {
         req->response = s_malloc0((unsigned long)(length) + 10);
         position = req->response;
      }
      req->resp_len = 0;
   }
   if (req->response) {
      position = req->response + req->resp_len;
      if (req->resp_len + sz_ > (unsigned long)length) {
         req_->bits |= HTTP_UNEXPECTED_RECV;
         // assert(0);
         lwqq_puts("[http unexpected]\n");
         return 0;
      }
   } else {
      struct trunk_entry* trunk = s_malloc0(sizeof(*trunk));
      trunk->size = sz_;
      trunk->trunk = s_malloc0(sz_);
      position = trunk->trunk;
      SIMPLEQ_INSERT_TAIL(&req_->trunks, trunk, entries);
   }
   memcpy(position, ptr, sz_);
   req->resp_len += sz_;
   return sz_;
}
static void composite_trunks(LwqqHttpRequest* req)
{
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   if (SIMPLEQ_EMPTY(&req_->trunks))
      return;
   size_t size = 0;
   struct trunk_entry* trunk;
   SIMPLEQ_FOREACH(trunk, &req_->trunks, entries) { size += trunk->size; }
   req->response = s_malloc0(size + 10);
   req->resp_len = 0;
   while ((trunk = SIMPLEQ_FIRST(&req_->trunks))) {
      SIMPLEQ_REMOVE_HEAD(&req_->trunks, entries);
      memcpy(req->response + req->resp_len, trunk->trunk, trunk->size);
      req->resp_len += trunk->size;
      s_free(trunk->trunk);
      s_free(trunk);
   }
}
#endif
// clean states between two curl request
static void http_clean(LwqqHttpRequest* req)
{
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
#ifdef HAVE_OPEN_MEMSTREAM
   if (req_->mem_buf)
      fclose(req_->mem_buf);
   req_->mem_buf = NULL;
#else
   composite_trunks(req);
#endif
   s_free(req->response);
   req->resp_len = 0;
   req->http_code = 0;
   curl_slist_free_all(req->recv_head);
   req->recv_head = NULL;
   req_->bits &= ~(HTTP_UNEXPECTED_RECV | HTTP_FORCE_CANCEL);
}
static void http_reset(LwqqHttpRequest* req)
// clean and reset between two call do_request_*
{
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   http_clean(req);
   req_->retry_ = req->retry;
   lwqq_http_set_option(req, LWQQ_HTTP_TIMEOUT, req_->timeout);
}

static void lwqq_http_set_header(LwqqHttpRequest* request, const char* name,
                                 const char* value)
{
   if (!request->req || !name || !value)
      return;
   // use libcurl internal cookie engine
   if (strcmp(name, "Cookie") == 0)
      return;

   size_t name_len = strlen(name);
   size_t value_len = strlen(value);
   char* opt = s_malloc(name_len + value_len + 3);

   strcpy(opt, name);
   opt[name_len] = ':';
   // need a blank space
   opt[name_len + 1] = ' ';
   strcpy(opt + name_len + 2, value);

   int use_old = 0;
   struct curl_slist* list = request->header;
   while (list) {
      if (!strncmp(list->data, name, strlen(name))) {
         s_free(list->data);
         list->data = s_strdup(opt);
         use_old = 1;
         break;
      }
      list = list->next;
   }
   if (!use_old) {
      request->header
          = curl_slist_append((struct curl_slist*)request->header, opt);
   }

   curl_easy_setopt(request->req, CURLOPT_HTTPHEADER, request->header);
   s_free(opt);
}

void lwqq_http_set_default_header(LwqqHttpRequest* request)
{
   lwqq_http_set_header(request, "User-Agent", LWQQ_HTTP_USER_AGENT);
   lwqq_http_set_header(request, "Accept", "*/*"
                        //",text/html, application/xml;q=0.9, "
                        //"application/xhtml+xml, image/png, image/jpeg, "
                        //"image/gif, image/x-xbitmap,;q=0.1"
                        );
   lwqq_http_set_header(request, "Accept-Language", "zh-cn,zh;q=0.9,en;q=0.8");
   lwqq_http_set_header(request, "Accept-Encoding", "gzip,deflate,x-gzip"
                        //"identity, *;q=0"
                        );
   lwqq_http_set_header(request, "Connection", "keep-alive");
}

static const char* lwqq_http_get_header(LwqqHttpRequest* request,
                                        const char* name)
{
   if (!name) {
      lwqq_log(LOG_ERROR, "Invalid parameter\n");
      return NULL;
   }

   const char* h = NULL;
   struct curl_slist* list = request->recv_head;
   while (list != NULL) {
      if (strncmp(name, list->data, strlen(name)) == 0) {
         h = list->data + strlen(name) + 2;
         break;
      }
      list = list->next;
   }

   return h;
}

char* lwqq_http_get_cookie(LwqqHttpRequest* req, const char* name)
{
   if (!name) {
      lwqq_log(LOG_ERROR, "Invalid parameter\n");
      return NULL;
   }
   struct curl_slist* list, *cookie;
   curl_easy_getinfo(req->req, CURLINFO_COOKIELIST, &cookie);
   list = cookie;
   char* n, *v = NULL;
   while (list != NULL) {
      v = strrchr(list->data, '\t') + 1;
      n = v - 2;
      while (n--, *n != '\t')
         ;
      n++;
      if (v - n - 1 == strlen(name) && strncmp(name, n, v - n - 1) == 0) {
         break;
      }
      list = list->next;
   }
   char* res = s_strdup(v);
   curl_slist_free_all(cookie);
   return res;
}
void lwqq_http_set_cookie(LwqqHttpRequest* req, const char* name,
                          const char* val, int store)
{
   if (!name) {
      lwqq_log(LOG_ERROR, "Invalid parameter\n");
      return;
   }
   if (!val)
      val = "";
   char buf[1024];
   CURLcode ret = 0;
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;

   if (store) {
      snprintf(buf, sizeof(buf), "Set-Cookie: %s=%s;", name, val);
      ret = curl_easy_setopt(req->req, CURLOPT_COOKIELIST, buf);
   } else {
      snprintf(buf, sizeof(buf), "%s %s=%s;", req_->cookie ?: "", name, val);
      lwqq_override(req_->cookie, strdup(buf));
      ret = curl_easy_setopt(req->req, CURLOPT_COOKIE, req_->cookie);
   }
   if (ret != CURLE_OK)
      lwqq_log(LOG_WARNING, "unable set cookie:%s", curl_easy_strerror(ret));
}
/**
 * Free Http Request
 *
 * @param request
 */
LWQQ_EXPORT
int lwqq_http_request_free(LwqqHttpRequest* request)
{
   if (!request)
      return 0;
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)request;

   if (request) {
      http_clean(request);
      s_free(request->response);
      curl_slist_free_all(request->header);
      curl_slist_free_all(request->recv_head);
      curl_formfree(request->form_start);
      if (request->req) {
         curl_easy_cleanup(request->req);
      }
      s_free(req_->cookie);
      s_free(request);
   }
   return 0;
}

static size_t write_header(void* ptr, size_t size, size_t nmemb, void* userdata)
{
   LwqqHttpRequest* request = (LwqqHttpRequest*)userdata;
#if 0
	char* str = (char*)ptr;

	long http_code = 0;
	curl_easy_getinfo(request->req,CURLINFO_RESPONSE_CODE,&http_code);
	//this is a redirection. ignore it.
	if(http_code == 301||http_code == 302){
		if(strncmp(str,"Location",strlen("Location"))==0){
			const char* location = str+strlen("Location: ");
			request->location = s_strdup(location);
			int len = strlen(request->location);
			//remove the last \r\n
			request->location[len-1] = 0;
			request->location[len-2] = 0;
			lwqq_verbose(3,"Location: %s\n",request->location);
		}
		return size*nmemb;
	}
#endif
   request->recv_head = curl_slist_append(request->recv_head, (char*)ptr);
   return size * nmemb;
}
static int curl_debug_redirect(CURL* h, curl_infotype t, char* msg, size_t len,
                               void* data)
{
   static char buffer[8192 * 10];
   size_t sz = sizeof(buffer) - 1;
   sz = sz > len ? sz : len;
   strncpy(buffer, msg, sz);
   buffer[sz] = 0;
   lwqq_verbose(3, "%s", buffer);
   return 0;
}
/**
 * Create a new Http request instance
 *
 * @param uri Request service from
 *
 * @return
 */
LWQQ_EXPORT
LwqqHttpRequest* lwqq_http_request_new(const char* uri)
{
   if (!uri) {
      return NULL;
   }

   LwqqHttpRequest* request;
   LwqqHttpRequest_* req_;
   request = s_malloc0(sizeof(LwqqHttpRequest_));
   req_ = (LwqqHttpRequest_*)request;
   req_->timeout = 15;

   request->req = curl_easy_init();
   request->retry = LWQQ_RETRY_VALUE;
   if (!request->req) {
      /* Seem like request->req must be non null. FIXME */
      goto failed;
   }
   if (curl_easy_setopt(request->req, CURLOPT_URL, uri) != 0) {
      lwqq_log(LOG_WARNING, "Invalid uri: %s\n", uri);
      goto failed;
   }
   curl_easy_setopt(request->req, CURLOPT_HEADERFUNCTION, write_header);
   curl_easy_setopt(request->req, CURLOPT_HEADERDATA, request);
   curl_easy_setopt(request->req, CURLOPT_NOSIGNAL, 1);
   curl_easy_setopt(request->req, CURLOPT_FOLLOWLOCATION, 1);
   curl_easy_setopt(request->req, CURLOPT_CONNECTTIMEOUT, 20);
   // set normal operate timeout to 30.official value.
   // curl_easy_setopt(request->req,CURLOPT_TIMEOUT,30);
   // low speed: 5B/s
   curl_easy_setopt(request->req, CURLOPT_LOW_SPEED_LIMIT, 8 * 5);
   curl_easy_setopt(request->req, CURLOPT_LOW_SPEED_TIME, req_->timeout);
   curl_easy_setopt(request->req, CURLOPT_SSL_VERIFYPEER, 0);
   curl_easy_setopt(request->req, CURLOPT_SSL_VERIFYHOST, 0);
   curl_easy_setopt(request->req, CURLOPT_DEBUGFUNCTION, curl_debug_redirect);
   curl_easy_setopt(request->req, CURLOPT_DNS_CACHE_TIMEOUT, -1);
   curl_easy_setopt(request->req, CURLOPT_SSLVERSION,
                    CURL_SSLVERSION_TLSv1); // force using tls v1.1
   request->do_request = lwqq_http_do_request;
   request->do_request_async = lwqq_http_do_request_async;
   request->set_header = lwqq_http_set_header;
   request->get_header = lwqq_http_get_header;
   request->add_form = lwqq_http_add_form;
   request->add_file_content = lwqq_http_add_file_content;
   return request;

failed:
   if (request) {
      lwqq_http_request_free(request);
   }
   return NULL;
}

static char* unzlib(const char* source, int len, int* total, int isgzip)
{
#define CHUNK 16 * 1024
   int ret;
   unsigned have;
   z_stream strm;
   unsigned char out[CHUNK];
   int totalsize = 0;
   char* dest = NULL;

   if (!source || len <= 0 || !total)
      return NULL;

   /* allocate inflate state */
   strm.zalloc = Z_NULL;
   strm.zfree = Z_NULL;
   strm.opaque = Z_NULL;
   strm.avail_in = 0;
   strm.next_in = Z_NULL;

   if (isgzip) {
      /**
       * 47 enable zlib and gzip decoding with automatic header detection,
       * So if the format of compress data is gzip, we need passed it to
       * inflateInit2
       */
      ret = inflateInit2(&strm, 47);
   } else {
      ret = inflateInit(&strm);
   }

   if (ret != Z_OK) {
      lwqq_log(LOG_ERROR, "Init zlib error\n");
      return NULL;
   }

   strm.avail_in = len;
   strm.next_in = (Bytef*)source;

   do {
      strm.avail_out = CHUNK;
      strm.next_out = out;
      ret = inflate(&strm, Z_NO_FLUSH);
      switch (ret) {
      case Z_STREAM_END:
         break;
      case Z_BUF_ERROR:
         lwqq_log(LOG_ERROR, "Unzlib error\n");
         break;
      case Z_NEED_DICT:
         ret = Z_DATA_ERROR; /* and fall through */
         break;
      case Z_DATA_ERROR:
      case Z_MEM_ERROR:
      case Z_STREAM_ERROR:
         lwqq_log(LOG_ERROR, "Ungzip stream error!", strm.msg);
         inflateEnd(&strm);
         goto failed;
      }
      have = CHUNK - strm.avail_out;
      totalsize += have;
      dest = s_realloc(dest, totalsize + 1);
      memcpy(dest + totalsize - have, out, have);
      dest[totalsize] = 0;
   } while (strm.avail_out == 0);

   /* clean up and return */
   (void)inflateEnd(&strm);
   if (ret != Z_STREAM_END) {
      goto failed;
   }
   *total = totalsize;
   return dest;

failed:
   if (dest) {
      s_free(dest);
   }
   lwqq_log(LOG_ERROR, "Unzip error\n");
   return NULL;
}

static char* ungzip(const char* source, int len, int* total)
{
   return unzlib(source, len, total, 1);
}

/**
 * Create a default http request object using default http header.
 *
 * @param url Which your want send this request to
 * @param err This parameter can be null, if so, we dont give thing
 *        error information.
 *
 * @return Null if failed, else a new http request object
 */
LwqqHttpRequest* lwqq_http_create_default_request(LwqqClient* lc,
                                                  const char* url,
                                                  LwqqErrorCode* err)
{
   LwqqHttpRequest* req;

   if (!url) {
      if (err)
         *err = LWQQ_EC_ERROR;
      return NULL;
   }

   req = lwqq_http_request_new(url);

   if (!req) {
      lwqq_log(LOG_ERROR, "Create request object for url: %s failed\n", url);
      if (err)
         *err = LWQQ_EC_ERROR;
      return NULL;
   }
   lwqq_http_set_default_header(req);

   LwqqHttpHandle* h = lwqq_get_http_handle(lc);
   LwqqHttpHandle_* h_ = (LwqqHttpHandle_*)h;
   curl_easy_setopt(req->req, CURLOPT_SHARE, h_->share);
   lwqq_http_proxy_apply(h, req);
   req->lc = lc;
   return req;
}

/************************************************************************/
/* Those Code for async API */

static void uncompress_response(LwqqHttpRequest* req)
{
   char* outdata;
   char** resp = &req->response;
   int total = 0;

   outdata = ungzip(*resp, req->resp_len, &total);
   if (!outdata)
      return;

   s_free(*resp);
   /* Update response data to uncompress data */
   *resp = outdata;
   req->resp_len = total;
}

// do some setting before a curl process complete
static void curl_network_begin(LwqqHttpRequest* req)
{
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
#ifdef HAVE_OPEN_MEMSTREAM
   curl_easy_setopt(req->req, CURLOPT_WRITEFUNCTION, NULL);
   req_->mem_buf = open_memstream(&req->response, &req->resp_len);
   curl_easy_setopt(req->req, CURLOPT_WRITEDATA, req_->mem_buf);
#else
   SIMPLEQ_INIT(&req_->trunks);
   curl_easy_setopt(req->req, CURLOPT_WRITEFUNCTION, write_content);
   curl_easy_setopt(req->req, CURLOPT_WRITEDATA, req);
#endif
}
// do some setting after a curl process complete
static void curl_network_complete(LwqqHttpRequest* req)
{
   long http_code = 0;
   curl_easy_getinfo(req->req, CURLINFO_RESPONSE_CODE, &http_code);
   req->http_code = http_code;

#ifdef HAVE_OPEN_MEMSTREAM
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   if (req_->mem_buf)
      fclose(req_->mem_buf);
   req_->mem_buf = NULL;
#else
   composite_trunks(req);
#endif

   /* NB: *response may null */
   if (req->response != NULL) {
      /* Uncompress data here if we have a Content-Encoding header */
      const char* enc_type = lwqq_http_get_header(req, "Content-Encoding");
      if (enc_type && strstr(enc_type, "gzip")) {
         uncompress_response(req);
      }
   }
}

static void async_complete(D_ITEM* conn)
{
   LwqqHttpRequest* request = conn->req;

   curl_network_complete(request);

   if (!lwqq_client_valid(request->lc))
      goto cleanup;
   int res = 0;
   // record error when network communication
   if (conn->req)
      conn->req->err = conn->event->result;
   vp_do(conn->cmd, &res);
   // copy out error code internal
   conn->event->result = res;
   lwqq_async_event_finish(conn->event);
cleanup:
   s_free(conn);
}
static int set_error_code(LwqqHttpRequest* req, CURLcode err, LwqqErrorCode* ec)
{
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   if (err == CURLE_ABORTED_BY_CALLBACK && req_->bits & HTTP_FORCE_CANCEL)
      req_->retry_ = 0;
   if (err == CURLE_TOO_MANY_REDIRECTS || err == CURLE_COULDNT_RESOLVE_HOST)
      req_->retry_ = 0;
   req_->retry_--;
   if (req_->tmo_inc > 0)
      curl_easy_setopt(req->req, CURLOPT_LOW_SPEED_TIME,
                       req_->timeout
                       + req_->tmo_inc * (req->retry - req_->retry_));
   if (req_->retry_ >= 0)
      return 1;
   *ec = errno_map(err);
   return 0;
}
static void check_multi_info(GLOBAL* g)
{
   CURLMsg* msg = NULL;
   int msgs_left;
   D_ITEM* conn;
   LwqqHttpRequest* req;
   LwqqAsyncEvent* ev;
   CURL* easy;
   CURLcode ret;

   // printf("still_running:%d\n",g->still_running);
   while ((msg = curl_multi_info_read(g->multi, &msgs_left))) {
      if (msg->msg == CURLMSG_DONE) {
         easy = msg->easy_handle;
         ret = msg->data.result;
         char* pridat = NULL;
         // avoid warnning
         curl_easy_getinfo(easy, CURLINFO_PRIVATE, &pridat);
         conn = (D_ITEM*)pridat;
         req = conn->req;
         ev = conn->event;
         if (ret != CURLE_OK) {
            lwqq_log(LOG_WARNING, "async retcode:%d %s\n", ret,
                     curl_easy_strerror(ret));
            LwqqErrorCode ec;
            if (set_error_code(req, ret, &ec)) {
               // re add it to libcurl
               curl_multi_remove_handle(g->multi, easy);
               http_clean(req);
               TAILQ_REMOVE(&global.conn_link, conn, entries);
               TAILQ_INSERT_TAIL(&global.add_link, conn, entries);
               global.conn_length--;
               lwqq_log(LOG_WARNING, "retry left:%d\n",
                        ((LwqqHttpRequest_*)req)->retry_);
               continue;
            }
            ev->result = ec;
         }

         curl_multi_remove_handle(g->multi, easy);
         TAILQ_REMOVE(&global.conn_link, conn, entries);

         global.conn_length--;

         LwqqClient* lc = conn->req->lc;

         //执行完成时候的回调
         if (lwqq_client_valid(lc))
            lwqq_client_dispatch(lc, _C_(p, async_complete, conn));
      }
   }
   delay_add_handle();
}
static void timer_cb(LwqqAsyncTimerHandle timer, void* data)
{
   //这个表示有超时任务出现.
   GLOBAL* g = data;
   // printf("timeout_come\n");

   if (!g->multi) {
      lwqq_async_timer_stop(timer);
      return;
   }
   curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0,
                            &g->still_running);
   check_multi_info(g);
   // this is inner timeout
   // always keep it
   lwqq_async_timer_repeat(timer);
}
static int multi_timer_cb(CURLM* multi, long timeout_ms, void* userp)
{
   // this function call only when timeout clock '''changed'''.
   // called by curl
   GLOBAL* g = userp;
   // printf("timer_cb:%ld\n",timeout_ms);
   lwqq_async_timer_stop(g->timer_event);
   if (timeout_ms > 0) {
      // change time clock
      lwqq_async_timer_watch(g->timer_event, timeout_ms, timer_cb, g);
   } else {
      // keep time clock
      timer_cb(g->timer_event, g);
   }
   // close time clock
   // this should always return 0 this is curl!!
   return 0;
}
static void event_cb(LwqqAsyncIoHandle io, int fd, int revents, void* data)
{
   GLOBAL* g = data;

   int action = (revents & LWQQ_ASYNC_READ ? CURL_POLL_IN : 0)
                | (revents & LWQQ_ASYNC_WRITE ? CURL_POLL_OUT : 0);
   curl_multi_socket_action(g->multi, fd, action, &g->still_running);
   check_multi_info(g);
   if (g->still_running <= 0) {
      lwqq_async_timer_stop(g->timer_event);
   }
}
static void setsock(S_ITEM* f, curl_socket_t s, CURL* e, int act, GLOBAL* g)
{
   int kind = ((act & CURL_POLL_IN) ? LWQQ_ASYNC_READ : 0)
              | ((act & CURL_POLL_OUT) ? LWQQ_ASYNC_WRITE : 0);

   f->sockfd = s;
   f->action = act;
   f->easy = e;
   if (f->evset)
      lwqq_async_io_stop(f->ev);
   // since read+write works fine. we find out 'kind' not worked when have time
   // lwqq_async_io_watch(&f->ev,f->sockfd,LWQQ_ASYNC_READ|LWQQ_ASYNC_WRITE,event_cb,g);
   // set both direction may cause upload file failed.so we restore it.
   lwqq_async_io_watch(f->ev, f->sockfd, kind, event_cb, g);

   f->evset = 1;
}
static int sock_cb(CURL* e, curl_socket_t s, int what, void* cbp, void* sockp)
{
   S_ITEM* si = (S_ITEM*)sockp;
   GLOBAL* g = cbp;

   if (what == CURL_POLL_REMOVE) {
      //清除socket关联对象
      if (si) {
         if (si->evset)
            lwqq_async_io_stop(si->ev);
         lwqq_async_io_free(si->ev);
         s_free(si);
         si = NULL;
      }
   } else {
      if (si == NULL) {
         //关联socket;
         si = s_malloc0(sizeof(*si));
         si->ev = lwqq_async_io_new();
         setsock(si, s, e, what, g);
         curl_multi_assign(g->multi, s, si);
      } else {
         //重新关联socket;
         setsock(si, s, e, what, g);
      }
   }
   return 0;
}

static void check_handle_and_add_to_conn_link()
{
   D_ITEM* di, *tvar;
   TAILQ_FOREACH_SAFE(di, &global.add_link, entries, tvar)
   {
      if (global.conn_length >= global.cache_size)
         break;
      TAILQ_REMOVE(&global.add_link, di, entries);
      TAILQ_INSERT_TAIL(&global.conn_link, di, entries);
      CURLMcode rc = curl_multi_add_handle(global.multi, di->req->req);
      global.conn_length++;

      if (rc != CURLM_OK) {
         lwqq_puts(curl_multi_strerror(rc));
      }
   }
}
#ifdef WITH_LIBEV
static void delay_add_handle_cb(LwqqAsyncIoHandle io, int fd, int act,
                                void* data)
{
   pthread_mutex_lock(&add_lock);
   // remove from pipe
   char buf[16];
   read(fd, buf, sizeof(buf));

   check_handle_and_add_to_conn_link();
   pthread_mutex_unlock(&add_lock);
}
static void delay_add_handle() { write(global.pipe_fd[1], "ok", 3); }
#else
static void delay_add_handle_cb(void* noused)
{
   pthread_mutex_lock(&add_lock);
   check_handle_and_add_to_conn_link();
   pthread_mutex_unlock(&add_lock);
}
static void delay_add_handle()
{
   lwqq_async_dispatch(_C_(p, delay_add_handle_cb, NULL));
}
#endif

static LwqqAsyncEvent* lwqq_http_do_request_async(LwqqHttpRequest* request,
                                                  int method, char* body,
                                                  LwqqCommand command)
{
   if (!request->req)
      return NULL;
   LwqqClient* lc = request->lc;

   if (LWQQ_SYNC_ENABLED(lc)) {
      LwqqHttpRequest_* req_ = (LwqqHttpRequest_*) request;
      req_->bits |= HTTP_SYNCED;
      LwqqAsyncEvent* ev = lwqq_async_event_new(request);
      int err = lwqq_http_do_request(request, method, body);
      vp_do(command, &err);
      lc->sync_result = ev->result = err;
      return ev;
   }

   /* Clear off last response */
   http_reset(request);

   /* Set http method */
   if (method == 0) {
   } else if (method == 1 && body) {
      curl_easy_setopt(request->req, CURLOPT_POST, 1);
      curl_easy_setopt(request->req, CURLOPT_COPYPOSTFIELDS, body);
   } else {
      lwqq_log(LOG_WARNING, "Wrong http method\n");
      goto failed;
   }

   if (global.multi == NULL) {
      lwqq_http_global_init();
   }

   curl_network_begin(request);

   D_ITEM* di = s_malloc0(sizeof(*di));
   curl_easy_setopt(request->req, CURLOPT_PRIVATE, di);
   di->cmd = command;
   di->req = request;
   di->event = lwqq_async_event_new(request);
   pthread_mutex_lock(&add_lock);
   TAILQ_INSERT_TAIL(&global.add_link, di, entries);
   pthread_mutex_unlock(&add_lock);
   delay_add_handle();
   return di->event;

failed:
   return NULL;
}
static int lwqq_http_do_request(LwqqHttpRequest* request, int method,
                                char* body)
{
   if (!request->req)
      return -1;
   CURLcode ret;
   http_reset(request);
   // mark this request is synced
   ((LwqqHttpRequest_*)request)->bits |= HTTP_SYNCED;
retry:
   ret = 0;

   /* Clear off last response */
   http_clean(request);

   /* Set http method */
   if (method == 0) {
   } else if (method == 1 && body) {
      curl_easy_setopt(request->req, CURLOPT_POST, 1);
      curl_easy_setopt(request->req, CURLOPT_COPYPOSTFIELDS, body);
   } else {
      lwqq_log(LOG_WARNING, "Wrong http method\n");
      return -1;
   }

   curl_network_begin(request);

   ret = curl_easy_perform(request->req);

   curl_network_complete(request);

   if (ret != CURLE_OK) {
      lwqq_log(LOG_ERROR, "do_request fail curlcode:%d\n", ret);
      LwqqErrorCode ec;
      if (set_error_code(request, ret, &ec)) {
         goto retry;
      }
      request->err = ec;
      return ec;
   }

   return 0;
}
static void share_lock(CURL* handle, curl_lock_data data,
                       curl_lock_access access, void* userptr)
{
   // this is shared access.
   // no need to lock it.
   if (access == CURL_LOCK_ACCESS_SHARED)
      return;
   LwqqHttpHandle_* h_ = userptr;
   int idx;
   switch (data) {
   case CURL_LOCK_DATA_DNS:
      idx = 0;
      break;
   case CURL_LOCK_DATA_CONNECT:
      idx = 1;
      break;
   case CURL_LOCK_DATA_SSL_SESSION:
      idx = 2;
      break;
   case CURL_LOCK_DATA_COOKIE:
      idx = 3;
      break;
   default:
      return;
   }
   pthread_mutex_lock(&h_->share_lock[idx]);
}
static void share_unlock(CURL* handle, curl_lock_data data, void* userptr)
{
   int idx;
   LwqqHttpHandle_* h_ = userptr;
   switch (data) {
   case CURL_LOCK_DATA_DNS:
      idx = 0;
      break;
   case CURL_LOCK_DATA_CONNECT:
      idx = 1;
      break;
   case CURL_LOCK_DATA_SSL_SESSION:
      idx = 2;
      break;
   case CURL_LOCK_DATA_COOKIE:
      idx = 3;
      break;
   default:
      return;
   }
   pthread_mutex_unlock(&h_->share_lock[idx]);
}
void lwqq_http_global_init()
{
   if (global.multi == NULL) {
      curl_global_init(CURL_GLOBAL_ALL);
      global.multi = curl_multi_init();
      curl_multi_setopt(global.multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
      curl_multi_setopt(global.multi, CURLMOPT_SOCKETDATA, &global);
      curl_multi_setopt(global.multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
      curl_multi_setopt(global.multi, CURLMOPT_TIMERDATA, &global);
      global.cache_size = 100;
      global.conn_length = 0;
      TAILQ_INIT(&global.conn_link);
      TAILQ_INIT(&global.add_link);

#ifndef WITHOUT_ASYNC
      global.timer_event = lwqq_async_timer_new();
      global.add_listener = lwqq_async_io_new();
#ifdef WITH_LIBEV
      pipe(global.pipe_fd);
      lwqq_async_io_watch(global.add_listener, global.pipe_fd[0],
                          LWQQ_ASYNC_READ, delay_add_handle_cb, NULL);
#endif

#endif
   }
}

static void safe_remove_link(LwqqClient* lc)
{
   D_ITEM* item, *tvar;
   CURL* easy;
   TAILQ_FOREACH_SAFE(item, &global.conn_link, entries, tvar)
   {
      if (lc && (item->req->lc != lc))
         continue;
      easy = item->req->req;
      curl_easy_pause(easy, CURLPAUSE_ALL);
      curl_multi_remove_handle(global.multi, easy);
   }
   if(LWQQ__ASYNC_IMPL(flags) & USE_THREAD){
      pthread_mutex_lock(&async_lock);
      // notify main thread have done jobs
      pthread_cond_signal(&async_cond);
      // wait and block sub thread to prevent do curl event
      // because this time main thread do curl clean job
      pthread_mutex_unlock(&async_lock);

      pthread_mutex_lock(&async_lock);
      pthread_cond_wait(&ev_block_cond, &async_lock);
      pthread_mutex_unlock(&async_lock);
   }
}

LWQQ_EXPORT
void lwqq_http_global_free(LwqqCleanUp cleanup)
{
   if (global.multi) {
      if(LWQQ__ASYNC_IMPL(flags) & USE_THREAD){
         pthread_mutex_lock(&async_lock);
         lwqq_async_dispatch(_C_(p, safe_remove_link, NULL));
         // wait sub thread remove all curl handle
         pthread_cond_wait(&async_cond, &async_lock);
         pthread_mutex_unlock(&async_lock);
      }else
         lwqq_async_dispatch(_C_(p, safe_remove_link, NULL));

      D_ITEM* item, *tvar;
      TAILQ_FOREACH_SAFE(item, &global.conn_link, entries, tvar)
      {
         TAILQ_REMOVE(&global.conn_link, item, entries);
         // let callback delete data
         item->req->err = item->event->result = LWQQ_EC_CANCELED;
         vp_do(item->cmd, NULL);
         if (cleanup == LWQQ_CLEANUP_WAITALL)
            lwqq_async_event_finish(item->event);
         s_free(item);
      }

      curl_multi_cleanup(global.multi);
      global.multi = NULL;
      lwqq_async_io_stop(global.add_listener);
      lwqq_async_io_free(global.add_listener);
#ifdef WITH_LIBEV
      close(global.pipe_fd[0]);
      close(global.pipe_fd[1]);
#endif
      lwqq_async_timer_stop(global.timer_event);
      lwqq_async_timer_free(global.timer_event);
      curl_global_cleanup();
      global.conn_length = 0;

      if(LWQQ__ASYNC_IMPL(flags) & USE_THREAD){
         pthread_mutex_lock(&async_lock);
         // notify sub thread we have already done curl clean job
         pthread_cond_signal(&ev_block_cond);
         pthread_mutex_unlock(&async_lock);
      }
   }
}

LWQQ_EXPORT
void lwqq_http_cleanup(LwqqClient* lc, LwqqCleanUp cleanup)
{
   if (lc && global.multi) {
      /**must dispatch safe_remove_link first
       * then vp_do(item->cmd) because vp_do might release memory
       */
      if(LWQQ__ASYNC_IMPL(flags) & USE_THREAD){
         pthread_mutex_lock(&async_lock);
         lwqq_async_dispatch(_C_(p, safe_remove_link, lc));
         // wait sub thread remove all curl handles
         pthread_cond_wait(&async_cond, &async_lock);
         pthread_mutex_unlock(&async_lock);
      }else
         lwqq_async_dispatch(_C_(p, safe_remove_link, lc));

      D_ITEM* item, *tvar;
      TAILQ_FOREACH_SAFE(item, &global.conn_link, entries, tvar)
      {
         if (item->req->lc != lc)
            continue;
         TAILQ_REMOVE(&global.conn_link, item, entries);
         item->req->err = item->event->result = LWQQ_EC_CANCELED;
         // let callback delete data
         vp_do(item->cmd, NULL);
         // XXX if cleanup == IGNORE, this would cause mem leak
         if (cleanup == LWQQ_CLEANUP_WAITALL) {
            lwqq_async_event_finish(item->event);
         }
         s_free(item);
      }
      if(LWQQ__ASYNC_IMPL(flags) & USE_THREAD){
         pthread_mutex_lock(&async_lock);
         // notify sub thread have done all curl clean job
         pthread_cond_signal(&ev_block_cond);
         pthread_mutex_unlock(&async_lock);
      }
   }
}

static void lwqq_http_add_form(LwqqHttpRequest* request, LWQQ_FORM form,
                               const char* name, const char* value)
{
   struct curl_httppost** post = (struct curl_httppost**)&request->form_start;
   struct curl_httppost** last = (struct curl_httppost**)&request->form_end;
   switch (form) {
   case LWQQ_FORM_FILE:
      curl_formadd(post, last, CURLFORM_COPYNAME, name, CURLFORM_FILE, value,
                   CURLFORM_END);
      break;
   case LWQQ_FORM_CONTENT:
      curl_formadd(post, last, CURLFORM_COPYNAME, name, CURLFORM_COPYCONTENTS,
                   value, CURLFORM_END);
      break;
   }
   curl_easy_setopt(request->req, CURLOPT_HTTPPOST, request->form_start);
}
static void lwqq_http_add_file_content(LwqqHttpRequest* request,
                                       const char* name, const char* filename,
                                       const void* data, size_t size,
                                       const char* extension)
{
   struct curl_httppost** post = (struct curl_httppost**)&request->form_start;
   struct curl_httppost** last = (struct curl_httppost**)&request->form_end;
   char* type = NULL;
   if (extension == NULL) {
      extension = strrchr(filename, '.');
      if (extension != NULL)
         extension++;
   }
   if (extension == NULL)
      type = NULL;
   else {
      if (strcmp(extension, "jpg") == 0 || strcmp(extension, "jpeg") == 0)
         type = "image/jpeg";
      else if (strcmp(extension, "png") == 0)
         type = "image/png";
      else if (strcmp(extension, "gif") == 0)
         type = "image/gif";
      else if (strcmp(extension, "bmp") == 0)
         type = "image/bmp";
      else
         type = NULL;
   }
   if (type == NULL) {
      curl_formadd(post, last, CURLFORM_COPYNAME, name, CURLFORM_BUFFER,
                   filename, CURLFORM_BUFFERPTR, data, CURLFORM_BUFFERLENGTH,
                   size, CURLFORM_END);
   } else {
      curl_formadd(post, last, CURLFORM_COPYNAME, name, CURLFORM_BUFFER,
                   filename, CURLFORM_BUFFERPTR, data, CURLFORM_BUFFERLENGTH,
                   size, CURLFORM_CONTENTTYPE, type, CURLFORM_END);
   }
   curl_easy_setopt(request->req, CURLOPT_HTTPPOST, request->form_start);
}

static int lwqq_http_progress_trans(void* d, double dt, double dn, double ut,
                                    double un)
{
   LwqqHttpRequest* req = d;
   LwqqHttpRequest_* req_ = d;
   if (req_->retry_ == 0 || req_->bits & HTTP_FORCE_CANCEL)
      return 1;
   time_t ct = time(NULL);
   if (ct <= req->last_prog)
      return 0;

   req->last_prog = ct;
   size_t now = dn + un;
   size_t total = dt + ut;
   return req->progress_func ? req->progress_func(req->prog_data, now, total)
                             : 0;
}

LWQQ_EXPORT
void lwqq_http_on_progress(LwqqHttpRequest* req, LwqqProgressFunc progress,
                           void* prog_data)
{
   if (!req)
      return;
   curl_easy_setopt(req->req, CURLOPT_PROGRESSFUNCTION,
                    lwqq_http_progress_trans);
   req->progress_func = progress;
   req->prog_data = prog_data;
   req->last_prog = time(NULL);
   curl_easy_setopt(req->req, CURLOPT_PROGRESSDATA, req);
   curl_easy_setopt(req->req, CURLOPT_NOPROGRESS, 0L);
}

LWQQ_EXPORT
void lwqq_http_set_option(LwqqHttpRequest* req, LwqqHttpOption opt, ...)
{
   if (!req)
      return;
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   va_list args;
   va_start(args, opt);
   unsigned long val = 0;
   switch (opt) {
   case LWQQ_HTTP_TIMEOUT:
      val = va_arg(args, unsigned long);
      curl_easy_setopt(req->req, CURLOPT_LOW_SPEED_TIME, val);
      req_->timeout = val;
      break;
   case LWQQ_HTTP_TIMEOUT_INCRE:
      val = va_arg(args, unsigned long);
      req_->tmo_inc = val;
      break;
   case LWQQ_HTTP_NOT_FOLLOW:
      curl_easy_setopt(req->req, CURLOPT_FOLLOWLOCATION, !va_arg(args, long));
      break;
   case LWQQ_HTTP_SAVE_FILE:
      curl_easy_setopt(req->req, CURLOPT_WRITEFUNCTION, NULL);
      curl_easy_setopt(req->req, CURLOPT_WRITEDATA, va_arg(args, FILE*));
      break;
   case LWQQ_HTTP_RESET_URL:
      curl_easy_setopt(req->req, CURLOPT_URL, va_arg(args, const char*));
      break;
   case LWQQ_HTTP_VERBOSE:
      curl_easy_setopt(req->req, CURLOPT_VERBOSE, va_arg(args, long));
      break;
   case LWQQ_HTTP_CANCELABLE:
      if (va_arg(args, long)&&req->progress_func == NULL)
         lwqq_http_on_progress(req, NULL, NULL);
      break;
   case LWQQ_HTTP_MAXREDIRS:
      curl_easy_setopt(req->req, CURLOPT_MAXREDIRS, va_arg(args, long));
      break;
   case LWQQ_HTTP_MAX_LINK:
      global.cache_size = va_arg(args, long);
      break;
   default:
      lwqq_log(LOG_ERROR, "unknow http option");
      break;
   }
   va_end(args);
}

LWQQ_EXPORT
void lwqq_http_cancel(LwqqHttpRequest* req)
{
   if (!req)
      return;
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   req_->retry_ = 0;
   req_->bits |= HTTP_FORCE_CANCEL;
}
LwqqHttpHandle* lwqq_http_handle_new()
{
   LwqqHttpHandle_* h_ = s_malloc0(sizeof(LwqqHttpHandle_));
   h_->parent.proxy.type = LWQQ_HTTP_PROXY_NOT_SET;
   h_->parent.ssl = 1;
   h_->share = curl_share_init();
   CURLSH* share = h_->share;
   curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
   curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
   // curl_share_setopt(share,CURLSHOPT_SHARE,CURL_LOCK_DATA_SSL_SESSION);
   curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
   curl_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock);
   curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock);
   curl_share_setopt(share, CURLSHOPT_USERDATA, h_);
   int i;
   for (i = 0; i < 4; i++)
      pthread_mutex_init(&h_->share_lock[i], NULL);
   return (LwqqHttpHandle*)h_;
}
void lwqq_http_handle_free(LwqqHttpHandle* http)
{
   if (http) {
      LwqqHttpHandle_* h_ = (LwqqHttpHandle_*)http;
      s_free(http->proxy.username);
      s_free(http->proxy.password);
      s_free(http->proxy.host);
      int i;
      for (i = 0; i < 4; i++)
         pthread_mutex_destroy(&h_->share_lock[i]);
      curl_share_cleanup(h_->share);
      s_free(http);
   }
}
void lwqq_http_proxy_apply(LwqqHttpHandle* handle, LwqqHttpRequest* req)
{
   CURL* c = req->req;
   char* v;
   long l;
   if (handle->proxy.type == LWQQ_HTTP_PROXY_NOT_SET) {
      return;
   } else if (handle->proxy.type == LWQQ_HTTP_PROXY_NONE) {
      curl_easy_setopt(c, CURLOPT_PROXY, "");
   } else {
      l = handle->proxy.type;
      curl_easy_setopt(c, CURLOPT_PROXYTYPE, proxy_map(l));
      v = handle->proxy.username;
      if (v)
         curl_easy_setopt(c, CURLOPT_PROXYUSERNAME, v);
      v = handle->proxy.password;
      if (v)
         curl_easy_setopt(c, CURLOPT_PROXYPASSWORD, v);
      v = handle->proxy.host;
      if (v)
         curl_easy_setopt(c, CURLOPT_PROXY, v);
      l = handle->proxy.port;
      if (l)
         curl_easy_setopt(c, CURLOPT_PROXYPORT, l);
   }
   curl_easy_setopt(c, CURLOPT_PROXYTYPE, handle->proxy.type);
}

const char* lwqq_http_get_url(LwqqHttpRequest* req)
{
   char* url = NULL;
   curl_easy_getinfo(req->req, CURLINFO_EFFECTIVE_URL, &url);
   return url;
}

int lwqq_http_is_synced(LwqqHttpRequest* req)
{
   LwqqHttpRequest_* req_ = (LwqqHttpRequest_*)req;
   return req_ ? (req_->bits & HTTP_SYNCED) : 0;
}

static void read_cookie(LwqqClient* lc, struct CookieExt* ext)
{
   // we just want pass cookie to curl, so we didn't need it do real network
   // request
   LwqqHttpRequest* req = lwqq_http_create_default_request(lc, WQQ_HOST, 0);
   curl_easy_setopt(req->req, CURLOPT_COOKIEFILE, ext->cookie_file);
   curl_easy_setopt(req->req, CURLOPT_CONNECT_ONLY, 1L);
   curl_easy_setopt(req->req, CURLOPT_CONNECTTIMEOUT_MS, 100L);
   lwqq_http_set_option(req, LWQQ_HTTP_TIMEOUT, 1);
   req->retry = 0;
   req->do_request_async(req, 0, "", _C_(p, lwqq_http_request_free, req));
}

static void write_cookie(LwqqClient* lc, struct CookieExt* ext)
{
   LwqqHttpRequest* req = lwqq_http_create_default_request(lc, WQQ_HOST, 0);
   curl_easy_setopt(req->req, CURLOPT_COOKIEJAR, ext->cookie_file);
   lwqq_http_request_free(req);
}

static void cookie_init(LwqqClient* lc, LwqqExtension* ext)
{
   struct CookieExt* ext_ = (struct CookieExt*)ext;
   ext_->login_b = lwqq_add_event(lc->events->start_login,
                                  _C_(2p, read_cookie, lc, ext_));
   ext_->login_c = lwqq_add_event(lc->events->login_complete,
                                  _C_(2p, write_cookie, lc, ext_));
   ext_->logout = lwqq_add_event(lc->events->start_logout,
                                 _C_(2p, write_cookie, lc, ext_));
   ext_->clean = lwqq_add_event(lc->events->ext_clean,
                                _C_(2p, lwqq_free_extension, lc, ext_));
}

static void cookie_remove(LwqqClient* lc, LwqqExtension* ext)
{
   struct CookieExt* ext_ = (struct CookieExt*)ext;
   vp_unlink(&lc->events->start_login, ext_->login_b);
   vp_unlink(&lc->events->login_complete, ext_->login_c);
   vp_unlink(&lc->events->start_logout, ext_->logout);
   vp_unlink(&lc->events->ext_clean, ext_->clean);
   s_free(ext_->cookie_file);
}

LwqqExtension* lwqq_make_cookie_extension(LwqqClient* lc, const char* filename)
{
   struct CookieExt* ext = s_malloc0(sizeof(*ext));
   ext->super.init = cookie_init;
   ext->super.remove = cookie_remove;
   ext->cookie_file = s_strdup(filename);
   return (LwqqExtension*)ext;
}
