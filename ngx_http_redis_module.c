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

    //���¼򵥵�Ӳ����ngx_http_upstream_conf_t�ṹ�еĸ���Ա������
//��ʱʱ�䶼��Ϊ1���ӡ���Ҳ��http�������ģ���Ĭ��ֵ
    mycf->upstream.connect_timeout = 60000;
    mycf->upstream.send_timeout = 60000;
    mycf->upstream.read_timeout = 60000;
    mycf->upstream.store_access = 0600;
    //ʵ����buffering�Ѿ������˽��Թ̶���С���ڴ���Ϊ��������ת�����ε�
//��Ӧ���壬���̶��������Ĵ�С����buffer_size�����bufferingΪ1
//�ͻ�ʹ�ø�����ڴ滺���������������ε���Ӧ���������ʹ��bufs.num��
//��������ÿ����������СΪbufs.size�����⻹��ʹ����ʱ�ļ�����ʱ�ļ���
//��󳤶�Ϊmax_temp_file_size
    mycf->upstream.buffering = 0;
    mycf->upstream.bufs.num = 8;
    mycf->upstream.bufs.size = ngx_pagesize;
    mycf->upstream.buffer_size = ngx_pagesize;
    mycf->upstream.busy_buffers_size = 2 * ngx_pagesize;
    mycf->upstream.temp_file_write_size = 2 * ngx_pagesize;
    mycf->upstream.max_temp_file_size = 1024 * 1024 * 1024;

    //upstreamģ��Ҫ��hide_headers��Ա����Ҫ��ʼ����upstream�ڽ���
//�����η��������صİ�ͷʱ�������
//ngx_http_upstream_process_headers��������hide_headers��Ա��
//��Ӧת�������ε�һЩhttpͷ�����أ������ｫ����Ϊ
//NGX_CONF_UNSET_PTR ����Ϊ����merge�ϲ����������ʹ��
//upstreamģ���ṩ��ngx_http_upstream_hide_headers_hash
//������ʼ��hide_headers ��Ա
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
    //����google���η�����������ܼ򵥣�����ģ����������������
//��/search?q=����URL��������������backendQueryLine�е�%V��ת��
//��ʽ���÷�����μ�4.4���еı�4-7
    static ngx_str_t backendQueryLine =
        ngx_string("*2\r\n$3\r\nget\r\n$4\r\nfuck\r\n*1\r\n$4\r\nquit\r\n");
    //�������ڴ���������ڴ棬��������ô���������������ѵ�����£�������
//��������������ʱ��������Ҫepoll��ε���send���Ͳ�����ɣ�
//��ʱ���뱣֤����ڴ治�ᱻ�ͷţ��������ʱ������ڴ�ᱻ�Զ��ͷţ�
//�����ڴ�й©�Ŀ���
    ngx_buf_t* b = ngx_create_temp_buf(r->pool, backendQueryLine.len);
    if (b == NULL)
        return NGX_ERROR;
    //lastҪָ�������ĩβ
    b->last = b->pos + backendQueryLine.len;

    //�����൱��snprintf��ֻ����֧��4.4���еı�4-7�г�������ת����ʽ
    ngx_snprintf(b->pos, backendQueryLine.len ,
                 (char*)backendQueryLine.data);
    // r->upstream->request_bufs��һ��ngx_chain_t�ṹ����������Ҫ
//���͸����η�����������
    r->upstream->request_bufs = ngx_alloc_chain_link(r->pool);
    if (r->upstream->request_bufs == NULL)
        return NGX_ERROR;

    // request_bufs����ֻ����1��ngx_buf_t������
    r->upstream->request_bufs->buf = b;
    r->upstream->request_bufs->next = NULL;

    r->upstream->request_sent = 0;
    r->upstream->header_sent = 0;
    // header_hash������Ϊ0
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

    //�����ҵ�redis���������������ÿ飬clcfò����location���ڵ�����
//�ṹ����ʵ��Ȼ����������main��srv����loc���������Ҳ����˵��ÿ��
//http{}��server{}��Ҳ����һ��ngx_http_core_loc_conf_t�ṹ��
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    //http����ڴ����û�������е�NGX_HTTP_CONTENT_PHASE�׶�ʱ�����
//���������������URI��redis���������ڵ����ÿ���ƥ�䣬�ͽ���������
//ʵ�ֵ�ngx_http_redis_handler���������������
    clcf->handler = ngx_http_redis_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_redis_handler(ngx_http_request_t *r)
{
    //���Ƚ���http�����Ľṹ��ngx_http_redis_ctx_t
    ngx_http_redis_ctx_t* myctx = ngx_http_get_module_ctx(r, ngx_http_redis_module);
    if (myctx == NULL)
    {
        myctx = ngx_palloc(r->pool, sizeof(ngx_http_redis_ctx_t));
        if (myctx == NULL)
        {
            return NGX_ERROR;
        }
        //���½����������������������
        ngx_http_set_ctx(r, myctx, ngx_http_redis_module);
    }
    //��ÿ1��Ҫʹ��upstream�����󣬱��������ֻ�ܵ���1��
//ngx_http_upstream_create�����������ʼ��r->upstream��Ա
    if (ngx_http_upstream_create(r) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_upstream_create() failed");
        return NGX_ERROR;
    }

    //�õ����ýṹ��ngx_http_redis_conf_t
    ngx_http_redis_conf_t  *mycf = (ngx_http_redis_conf_t  *) ngx_http_get_module_loc_conf(r, ngx_http_redis_module);
    ngx_http_upstream_t *u = r->upstream;
    //�����������ļ��еĽṹ��������r->upstream->conf��Ա
    u->conf = &mycf->upstream;
    //����ת������ʱʹ�õĻ�����
    u->buffering = mycf->upstream.buffering;

    //���´��뿪ʼ��ʼ��resolved�ṹ�壬�����������η������ĵ�ַ
    u->resolved = (ngx_http_upstream_resolved_t*) ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_pcalloc resolved error. %s.", strerror(errno));
        return NGX_ERROR;
    }
	
    static struct sockaddr_in backendSockAddr;
	
    
    //�������η�����redis
    backendSockAddr.sin_family = AF_INET;
    backendSockAddr.sin_port = htons((in_port_t) 6379);
    char* pDmsIP = "127.0.0.1"; //inet_ntoa(*(struct in_addr*) (pHost->h_addr_list[0]));
    backendSockAddr.sin_addr.s_addr = inet_addr(pDmsIP);
    myctx->backendServer.data = (u_char*)pDmsIP;
    myctx->backendServer.len = strlen(pDmsIP);

    //����ַ���õ�resolved��Ա��
    u->resolved->sockaddr = (struct sockaddr *)&backendSockAddr;
    u->resolved->socklen = sizeof(struct sockaddr_in);
    u->resolved->naddrs = 1;
	u->resolved->port = 6379;  //�������η������˿�

    //������������ʵ�ֵĻص�������Ҳ����5.3.3����5.3.5����ʵ�ֵ�3������
    u->create_request = redis_upstream_create_request;
    u->process_header = redis_process_status_line;
    u->finalize_request = redis_upstream_finalize_request;

    //������뽫count��Ա��1�����ɼ�5.1.5��
    r->main->count++;
    //����upstream
    ngx_http_upstream_init(r);
    //���뷵��NGX_DONE
    return NGX_DONE;
}


