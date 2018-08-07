#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct
{
    ngx_http_status_t           status;
    ngx_str_t					backendServer;

    ngx_int_t                  query_count;
    ngx_http_request_t        *request;
    int                        state;
    size_t                     chunk_size;
    size_t                     chunk_bytes_read;
    size_t                     chunks_read;
    size_t                     chunk_count;
} ngx_http_redis_ctx_t;

typedef struct
{
    ngx_http_upstream_conf_t   upstream;
    ngx_str_t                  literal_query; /* for redis_literal_raw_query */
    ngx_http_complex_value_t  *complex_query; /* for redis_raw_query */
    ngx_http_complex_value_t  *complex_query_count; /* for redis_raw_query */
    ngx_http_complex_value_t  *complex_target; /* for redis_pass */
    ngx_array_t               *queries; /* for redis_query */

} ngx_http_redis_conf_t;


static char *
ngx_http_redis(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_redis_handler(ngx_http_request_t *r);
static void* ngx_http_redis_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_redis_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static char *
ngx_http_redis_query(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t
redis_process_status_line(ngx_http_request_t *r);


static ngx_command_t  ngx_http_redis_commands[] =
{

    { ngx_string("redis_query"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_1MORE,
      ngx_http_redis_query,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

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



static char *
ngx_http_redis_query(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_redis_conf_t  *rlcf = conf;
    ngx_str_t                   *value;
    ngx_array_t                **query;
    ngx_uint_t                   n;
    ngx_http_complex_value_t   **arg;
    ngx_uint_t                   i;

    ngx_http_compile_complex_value_t         ccv;

    if (rlcf->literal_query.len) {
        return "conflicts with redis_literal_raw_query";
    }

    if (rlcf->complex_query) {
        return "conflicts with redis_raw_query";
    }

    if (rlcf->queries == NULL) {
        rlcf->queries = ngx_array_create(cf->pool, 1, sizeof(ngx_array_t *));

        if (rlcf->queries == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    query = ngx_array_push(rlcf->queries);
    if (query == NULL) {
        return NGX_CONF_ERROR;
    }

    n = cf->args->nelts - 1;

    *query = ngx_array_create(cf->pool, n, sizeof(ngx_http_complex_value_t *));

    if (*query == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    for (i = 1; i <= n; i++) {
        arg = ngx_array_push(*query);
        if (arg == NULL) {
            return NGX_CONF_ERROR;
        }

        *arg = ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
        if (*arg == NULL) {
            return NGX_CONF_ERROR;
        }

        if (value[i].len == 0) {
            ngx_memzero(*arg, sizeof(ngx_http_complex_value_t));
            continue;
        }

        ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
        ccv.cf = cf;
        ccv.value = &value[i];
        ccv.complex_value = *arg;

        if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

static void* ngx_http_redis_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_redis_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_redis_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->upstream.bufs.num = 0;
     *     conf->upstream.next_upstream = 0;
     *     conf->upstream.temp_path = NULL;
     *     conf->upstream.uri = { 0, NULL };
     *     conf->upstream.location = NULL;
     *     conf->complex_query = NULL;
     *     conf->literal_query = { 0, NULL };
     *     conf->queries = NULL;
     */

    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;

    conf->upstream.buffer_size = NGX_CONF_UNSET_SIZE;

    /* the hardcoded values */
    conf->upstream.cyclic_temp_file = 0;
    conf->upstream.buffering = 0;
    conf->upstream.ignore_client_abort = 1;
    conf->upstream.send_lowat = 0;
    conf->upstream.bufs.num = 0;
    conf->upstream.busy_buffers_size = 0;
    conf->upstream.max_temp_file_size = 0;
    conf->upstream.temp_file_write_size = 0;
    conf->upstream.intercept_errors = 1;
    conf->upstream.intercept_404 = 1;
    conf->upstream.pass_request_headers = 0;
    conf->upstream.pass_request_body = 0;

    return conf;
}



static char *ngx_http_redis_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_redis_conf_t *prev = parent;
    ngx_http_redis_conf_t *conf = child;

    ngx_conf_merge_msec_value(conf->upstream.connect_timeout,
                              prev->upstream.connect_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.send_timeout,
                              prev->upstream.send_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.read_timeout,
                              prev->upstream.read_timeout, 60000);

    ngx_conf_merge_size_value(conf->upstream.buffer_size,
                              prev->upstream.buffer_size,
                              (size_t) ngx_pagesize);

    ngx_conf_merge_bitmask_value(conf->upstream.next_upstream,
                                 prev->upstream.next_upstream,
                                 (NGX_CONF_BITMASK_SET
                                  |NGX_HTTP_UPSTREAM_FT_ERROR
                                  |NGX_HTTP_UPSTREAM_FT_TIMEOUT));

    if (conf->upstream.next_upstream & NGX_HTTP_UPSTREAM_FT_OFF) {
        conf->upstream.next_upstream = NGX_CONF_BITMASK_SET
                                       |NGX_HTTP_UPSTREAM_FT_OFF;
    }

    if (conf->upstream.upstream == NULL) {
        conf->upstream.upstream = prev->upstream.upstream;
    }

    if (conf->complex_query == NULL) {
        conf->complex_query = prev->complex_query;
    }

    if (conf->complex_query_count == NULL) {
        conf->complex_query_count = prev->complex_query_count;
    }

    if (conf->queries == NULL) {
        conf->queries = prev->queries;
    }

    if (conf->literal_query.data == NULL) {
        conf->literal_query.data = prev->literal_query.data;
        conf->literal_query.len = prev->literal_query.len;
    }

    return NGX_CONF_OK;
}


static size_t
ngx_get_num_size(uint64_t i)
{
    size_t          n = 0;

    do {
        i = i / 10;
        n++;
    } while (i > 0);

    return n;
}


ngx_int_t
ngx_http_redis_build_query(ngx_http_request_t *r, ngx_array_t *queries,
    ngx_buf_t **b)
{
    ngx_uint_t                       i, j;
    ngx_uint_t                       n;
    ngx_str_t                       *arg;
    ngx_array_t                     *args;
    size_t                           len;
    ngx_array_t                    **query_args;
    ngx_http_complex_value_t       **complex_arg;
    u_char                          *p;
    ngx_http_redis_conf_t      *rlcf;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_redis_module);

    query_args = rlcf->queries->elts;

    n = 0;
    for (i = 0; i < rlcf->queries->nelts; i++) {
        for (j = 0; j < query_args[i]->nelts; j++) {
            n++;
        }
    }

    args = ngx_array_create(r->pool, n, sizeof(ngx_str_t));

    if (args == NULL) {
        return NGX_ERROR;
    }

    len = 0;
    n = 0;

    for (i = 0; i < rlcf->queries->nelts; i++) {
        complex_arg = query_args[i]->elts;

        len += sizeof("*") - 1
             + ngx_get_num_size(query_args[i]->nelts)
             + sizeof("\r\n") - 1
             ;

        for (j = 0; j < query_args[i]->nelts; j++) {
            n++;

            arg = ngx_array_push(args);
            if (arg == NULL) {
                return NGX_ERROR;
            }

            if (ngx_http_complex_value(r, complex_arg[j], arg) != NGX_OK) {
                return NGX_ERROR;
            }

            len += sizeof("$") - 1
                 + ngx_get_num_size(arg->len)
                 + sizeof("\r\n") - 1
                 + arg->len
                 + sizeof("\r\n") - 1
                 ;
			
#if 1
			{
				char tempstr[1024] = {0};
				snprintf(tempstr, arg->len + strlen("the fucking query data: ") + 1,"the fucking query data: %s", arg->data);
				//debug script
				printf("\033[1;31m[File:%s]\033[0m\n", __FILE__);
				printf("\033[1;31m[Function:%s]\033[0m\n", __FUNCTION__);
				printf("\033[1;31m[Line:%d]\033[0m\n", __LINE__);
				printf("\033[1;31m[tempstr:%s]\033[0m\n", tempstr);
				printf("\n");
			}
#endif
        }
    }

    *b = ngx_create_temp_buf(r->pool, len);
    if (*b == NULL) {
        return NGX_ERROR;
    }

    p = (*b)->last;

    arg = args->elts;

    n = 0;
    for (i = 0; i < rlcf->queries->nelts; i++) {
        *p++ = '*';
        p = ngx_sprintf(p, "%uz", query_args[i]->nelts);
        *p++ = '\r'; *p++ = '\n';

        for (j = 0; j < query_args[i]->nelts; j++) {
            *p++ = '$';
            p = ngx_sprintf(p, "%uz", arg[n].len);
            *p++ = '\r'; *p++ = '\n';
            p = ngx_copy(p, arg[n].data, arg[n].len);
            *p++ = '\r'; *p++ = '\n';

            n++;
        }
    }

//    dd("query: %.*s", (int) (p - (*b)->pos), (*b)->pos);

    if (p - (*b)->pos != (ssize_t) len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "redis: redis_query buffer error %uz != %uz",
                      (size_t) (p - (*b)->pos), len);

        return NGX_ERROR;
    }

    (*b)->last = p;

    return NGX_OK;
}

static ngx_int_t
redis_upstream_create_request(ngx_http_request_t *r)
{
    ngx_buf_t                       *b;
    ngx_chain_t                     *cl;
    ngx_http_redis_conf_t      *rlcf;
    ngx_str_t                        query;
    ngx_str_t                        query_count;
    ngx_int_t                        rc;
    ngx_http_redis_ctx_t           *ctx;
    ngx_int_t                        n;

    ctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_redis_module);

    if (rlcf->queries) {
        ctx->query_count = rlcf->queries->nelts;

        rc = ngx_http_redis_build_query(r, rlcf->queries, &b);
        if (rc != NGX_OK) {
            return rc;
        }

    } else if (rlcf->literal_query.len == 0) {
        if (rlcf->complex_query == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "no redis query specified or the query is empty");

            return NGX_ERROR;
        }

        if (ngx_http_complex_value(r, rlcf->complex_query, &query)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        if (query.len == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the redis query is empty");

            return NGX_ERROR;
        }

        if (rlcf->complex_query_count == NULL) {
            ctx->query_count = 1;

        } else {
            if (ngx_http_complex_value(r, rlcf->complex_query_count,
                                       &query_count)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            if (query_count.len == 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "the N argument to redis_raw_queries is empty");

                return NGX_ERROR;
            }

            n = ngx_atoi(query_count.data, query_count.len);
            if (n == NGX_ERROR || n == 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "the N argument to redis_raw_queries is "
                              "invalid");

                return NGX_ERROR;
            }

            ctx->query_count = n;
        }

        b = ngx_create_temp_buf(r->pool, query.len);
        if (b == NULL) {
            return NGX_ERROR;
        }

        b->last = ngx_copy(b->pos, query.data, query.len);

    } else {
        ctx->query_count = 1;

        b = ngx_calloc_buf(r->pool);
        if (b == NULL) {
            return NGX_ERROR;
        }

        b->pos = rlcf->literal_query.data;
        b->last = b->pos + rlcf->literal_query.len;
        b->memory = 1;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    r->upstream->request_bufs = cl;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http redis request: \"%V\"", &rlcf->literal_query);

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
            //ctx->filter = ngx_http_redis_process_reply;
            break;

        default:
            buf.data = b->pos;
            buf.len = b->last - b->pos;

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "redis sent invalid response: \"%V\"", &buf);

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


