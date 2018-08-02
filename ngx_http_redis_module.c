#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct
{
    ngx_http_status_t           status;
    ngx_str_t					backendServer;
} ngx_http_redis_ctx_t;

typedef struct
{
    ngx_http_upstream_conf_t upstream;
} ngx_http_redis_conf_t;


static char *
ngx_http_redis(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_redis_handler(ngx_http_request_t *r);
static void* ngx_http_redis_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_redis_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_int_t
redis_process_status_line(ngx_http_request_t *r);


static ngx_str_t  ngx_http_proxy_hide_headers[] =
{
    ngx_string("Date"),
    ngx_string("Server"),
    ngx_string("X-Pad"),
    ngx_string("X-Accel-Expires"),
    ngx_string("X-Accel-Redirect"),
    ngx_string("X-Accel-Limit-Rate"),
    ngx_string("X-Accel-Buffering"),
    ngx_string("X-Accel-Charset"),
    ngx_null_string
};


static ngx_command_t  ngx_http_redis_commands[] =
{

    {
        ngx_string("redis"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_NOARGS,
        ngx_http_redis,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t  ngx_http_redis_module_ctx =
{
    NULL,                              /* preconfiguration */
    NULL,                  		/* postconfiguration */

    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */

    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */

    ngx_http_redis_create_loc_conf,       			/* create location configuration */
    ngx_http_redis_merge_loc_conf         			/* merge location configuration */
};

ngx_module_t  ngx_http_redis_module =
{
    NGX_MODULE_V1,
    &ngx_http_redis_module_ctx,           /* module context */
    ngx_http_redis_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void* ngx_http_redis_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_redis_conf_t  *mycf;

    mycf = (ngx_http_redis_conf_t  *)ngx_pcalloc(cf->pool, sizeof(ngx_http_redis_conf_t));
    if (mycf == NULL)
    {
        return NULL;
    }

    //以下简单的硬编码ngx_http_upstream_conf_t结构中的各成员，例如
//超时时间都设为1分钟。这也是http反向代理模块的默认值
    mycf->upstream.connect_timeout = 60000;
    mycf->upstream.send_timeout = 60000;
    mycf->upstream.read_timeout = 60000;
    mycf->upstream.store_access = 0600;
    //实际上buffering已经决定了将以固定大小的内存作为缓冲区来转发上游的
//响应包体，这块固定缓冲区的大小就是buffer_size。如果buffering为1
//就会使用更多的内存缓存来不及发往下游的响应，例如最多使用bufs.num个
//缓冲区、每个缓冲区大小为bufs.size，另外还会使用临时文件，临时文件的
//最大长度为max_temp_file_size
    mycf->upstream.buffering = 0;
    mycf->upstream.bufs.num = 8;
    mycf->upstream.bufs.size = ngx_pagesize;
    mycf->upstream.buffer_size = ngx_pagesize;
    mycf->upstream.busy_buffers_size = 2 * ngx_pagesize;
    mycf->upstream.temp_file_write_size = 2 * ngx_pagesize;
    mycf->upstream.max_temp_file_size = 1024 * 1024 * 1024;

    //upstream模块要求hide_headers成员必须要初始化（upstream在解析
//完上游服务器返回的包头时，会调用
//ngx_http_upstream_process_headers方法按照hide_headers成员将
//本应转发给下游的一些http头部隐藏），这里将它赋为
//NGX_CONF_UNSET_PTR ，是为了在merge合并配置项方法中使用
//upstream模块提供的ngx_http_upstream_hide_headers_hash
//方法初始化hide_headers 成员
    mycf->upstream.hide_headers = NGX_CONF_UNSET_PTR;
    mycf->upstream.pass_headers = NGX_CONF_UNSET_PTR;

    return mycf;
}


static char *ngx_http_redis_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_redis_conf_t *prev = (ngx_http_redis_conf_t *)parent;
    ngx_http_redis_conf_t *conf = (ngx_http_redis_conf_t *)child;

    ngx_hash_init_t             hash;
    hash.max_size = 100;
    hash.bucket_size = 1024;
    hash.name = "proxy_headers_hash";
    if (ngx_http_upstream_hide_headers_hash(cf, &conf->upstream,
                                            &prev->upstream, ngx_http_proxy_hide_headers, &hash)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
redis_upstream_create_request(ngx_http_request_t *r)
{
    //发往google上游服务器的请求很简单，就是模仿正常的搜索请求，
//以/search?q=…的URL来发起搜索请求。backendQueryLine中的%V等转化
//格式的用法，请参见4.4节中的表4-7
    static ngx_str_t backendQueryLine =
        ngx_string("*2\r\n$3\r\nget\r\n$4\r\nfuck\r\n*1\r\n$4\r\nquit\r\n");
    //必须由内存池中申请内存，这有两点好处：在网络情况不佳的情况下，向上游
//服务器发送请求时，可能需要epoll多次调度send发送才能完成，
//这时必须保证这段内存不会被释放；请求结束时，这段内存会被自动释放，
//降低内存泄漏的可能
    ngx_buf_t* b = ngx_create_temp_buf(r->pool, backendQueryLine.len);
    if (b == NULL)
        return NGX_ERROR;
    //last要指向请求的末尾
    b->last = b->pos + backendQueryLine.len;

    //作用相当于snprintf，只是它支持4.4节中的表4-7列出的所有转换格式
    ngx_snprintf(b->pos, backendQueryLine.len ,
                 (char*)backendQueryLine.data);
    // r->upstream->request_bufs是一个ngx_chain_t结构，它包含着要
//发送给上游服务器的请求
    r->upstream->request_bufs = ngx_alloc_chain_link(r->pool);
    if (r->upstream->request_bufs == NULL)
        return NGX_ERROR;

    // request_bufs这里只包含1个ngx_buf_t缓冲区
    r->upstream->request_bufs->buf = b;
    r->upstream->request_bufs->next = NULL;

    r->upstream->request_sent = 0;
    r->upstream->header_sent = 0;
    // header_hash不可以为0
    r->header_hash = 1;
    r->subrequest_in_memory = 0;
    return NGX_OK;
}

static ngx_int_t
redis_process_status_line(ngx_http_request_t *r)
{
    ngx_http_upstream_t         *u;
    ngx_http_redis_ctx_t       *ctx;
    ngx_buf_t                   *b;
    u_char                       chr;
    ngx_str_t                    buf;

    u = r->upstream;
    b = &u->buffer;

    if (b->last - b->pos < (ssize_t) sizeof(u_char)) {
        return NGX_AGAIN;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);
	ctx = ctx;
    /* the first char is the response header */

    chr = *b->pos;

    //dd("response header: %c (ascii %d)", chr, chr);

    switch (chr) {
        case '+':
        case '-':
        case ':':
        case '$':
        case '*':
            //ctx->filter = ngx_http_redis2_process_reply;
            break;

        default:
            buf.data = b->pos;
            buf.len = b->last - b->pos;

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "redis2 sent invalid response: \"%V\"", &buf);

            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

    u->headers_in.status_n = NGX_HTTP_OK;
    u->state->status = NGX_HTTP_OK;

    return NGX_OK;
}


static void
redis_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "redis_upstream_finalize_request");
}


static char *
ngx_http_redis(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    //首先找到redis配置项所属的配置块，clcf貌似是location块内的数据
//结构，其实不然，它可以是main、srv或者loc级别配置项，也就是说在每个
//http{}和server{}内也都有一个ngx_http_core_loc_conf_t结构体
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    //http框架在处理用户请求进行到NGX_HTTP_CONTENT_PHASE阶段时，如果
//请求的主机域名、URI与redis配置项所在的配置块相匹配，就将调用我们
//实现的ngx_http_redis_handler方法处理这个请求
    clcf->handler = ngx_http_redis_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_redis_handler(ngx_http_request_t *r)
{
    //首先建立http上下文结构体ngx_http_redis_ctx_t
    ngx_http_redis_ctx_t* myctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);
    if (myctx == NULL)
    {
        myctx = ngx_palloc(r->pool, sizeof(ngx_http_redis_ctx_t));
        if (myctx == NULL)
        {
            return NGX_ERROR;
        }
        //将新建的上下文与请求关联起来
        ngx_http_set_ctx(r, myctx, ngx_http_redis_module);
    }
    //对每1个要使用upstream的请求，必须调用且只能调用1次
//ngx_http_upstream_create方法，它会初始化r->upstream成员
    if (ngx_http_upstream_create(r) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_upstream_create() failed");
        return NGX_ERROR;
    }

    //得到配置结构体ngx_http_redis_conf_t
    ngx_http_redis_conf_t  *mycf = (ngx_http_redis_conf_t  *) ngx_http_get_module_loc_conf(r, ngx_http_redis_module);
    ngx_http_upstream_t *u = r->upstream;
    //这里用配置文件中的结构体来赋给r->upstream->conf成员
    u->conf = &mycf->upstream;
    //决定转发包体时使用的缓冲区
    u->buffering = mycf->upstream.buffering;

    //以下代码开始初始化resolved结构体，用来保存上游服务器的地址
    u->resolved = (ngx_http_upstream_resolved_t*) ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_pcalloc resolved error. %s.", strerror(errno));
        return NGX_ERROR;
    }
	
    static struct sockaddr_in backendSockAddr;
	
    
    //访问上游服务器redis
    backendSockAddr.sin_family = AF_INET;
    backendSockAddr.sin_port = htons((in_port_t) 6379);
    char* pDmsIP = "127.0.0.1"; //inet_ntoa(*(struct in_addr*) (pHost->h_addr_list[0]));
    backendSockAddr.sin_addr.s_addr = inet_addr(pDmsIP);
    myctx->backendServer.data = (u_char*)pDmsIP;
    myctx->backendServer.len = strlen(pDmsIP);

    //将地址设置到resolved成员中
    u->resolved->sockaddr = (struct sockaddr *)&backendSockAddr;
    u->resolved->socklen = sizeof(struct sockaddr_in);
    u->resolved->naddrs = 1;
	u->resolved->port = 6379;  //设置上游服务器端口

    //设置三个必须实现的回调方法，也就是5.3.3节至5.3.5节中实现的3个方法
    u->create_request = redis_upstream_create_request;
    u->process_header = redis_process_status_line;
    u->finalize_request = redis_upstream_finalize_request;

    //这里必须将count成员加1，理由见5.1.5节
    r->main->count++;
    //启动upstream
    ngx_http_upstream_init(r);
    //必须返回NGX_DONE
    return NGX_DONE;
}


