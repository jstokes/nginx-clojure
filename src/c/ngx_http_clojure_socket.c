/*
 *  Copyright (C) Zhang,Yuexiang (xfeep)
 */

#include <ngx_config.h>
#include "ngx_http_clojure_socket.h"
#include "ngx_http_clojure_jvm.h"

//static JavaVM *jvm = NULL;
static JNIEnv *jvm_env = NULL;
static jclass nc_socket_class;
static jmethodID nc_socket_handler_read_mid;
static jmethodID nc_socket_handler_write_mid;
static jmethodID nc_socket_handler_connect_mid;
static jmethodID nc_socket_handler_release_mid;
static int ngx_http_clojure_init_socket_flag = NGX_HTTP_CLOJURE_JVM_ERR;

static void ngx_http_clojure_socket_upstream_handler(ngx_event_t *ev);
static void ngx_http_clojure_socket_upstream_read_handler(ngx_event_t *ev);
static void ngx_http_clojure_socket_upstream_write_handler(ngx_event_t *ev);
static void ngx_http_clojure_socket_upstream_empty_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc);
static void ngx_http_clojure_socket_upstream_connect_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc);
static ngx_int_t ngx_http_clojure_socket_upstream_test_connect(ngx_connection_t *c);
static void ngx_http_clojure_socket_upstream_finalize(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc);
static void ngx_http_clojure_socket_upstream_connect_1(ngx_http_clojure_socket_upstream_t *u);


static ngx_int_t ngx_http_clojure_socket_upstream_test_connect(ngx_connection_t *c) {
	int err;
	socklen_t len;

#if (NGX_HAVE_KQUEUE)

	if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
		if (c->write->pending_eof || c->read->pending_eof) {
			if (c->write->pending_eof) {
				err = c->write->kq_errno;

			} else {
				err = c->read->kq_errno;
			}

			c->log->action = "connecting to upstream";
			(void) ngx_connection_error(c, err,
					"kevent() reported that connect() failed");
			return NGX_ERROR;
		}

	} else
#endif
	{
		err = 0;
		len = sizeof(int);

		/*
		 * BSDs and Linux return 0 and set a pending error in err
		 * Solaris returns -1 and sets errno
		 */

		if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
				== -1) {
			err = ngx_errno;
		}

		if (err) {
			c->log->action = "connecting to upstream";
			(void) ngx_connection_error(c, err, "connect() failed");
			return NGX_ERROR;
		}
	}

	return NGX_OK;
}

static void ngx_http_clojure_socket_upstream_finalize(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc) {

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, u->pool->log, 0,
	                   "finalize clojure_socket_upstream: %i", sc);

	if (u->resolved && u->resolved->ctx) {
		ngx_resolve_name_done(u->resolved->ctx);
		u->resolved->ctx = NULL;
	}

	if (u->socket_upstream_finalize) {
		u->socket_upstream_finalize(u, sc);
	}


    if (u->peer.free && u->peer.sockaddr) {
        u->peer.free(&u->peer, u->peer.data, 0);
        u->peer.sockaddr = NULL;
    }


    if (u->peer.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, u->pool->log, 0,
                       "close clojure_socket_upstream connection: %d",
                       u->peer.connection->fd);

        if (u->peer.connection->pool) {
            ngx_destroy_pool(u->peer.connection->pool);
        }

        ngx_close_connection(u->peer.connection);
    }

    u->peer.connection = NULL;

    ngx_destroy_pool(u->pool);

}

ngx_http_clojure_socket_upstream_t *ngx_http_clojure_socket_upstream_create(size_t pool_size, ngx_log_t *log) {
	ngx_pool_t *pool = ngx_create_pool(pool_size, log);
	if (pool == NULL) {
		return NULL;
	}

	ngx_http_clojure_socket_upstream_t *u = ngx_pcalloc(pool, sizeof(ngx_http_clojure_socket_upstream_t));
	if (u == NULL) {
		ngx_destroy_pool(pool);
		return NULL;
	}

	u->pool = pool;

	u->resolved = ngx_pcalloc(pool, sizeof(ngx_http_upstream_resolved_t));
	if (u->resolved == NULL) {
		ngx_destroy_pool(pool);
		return NULL;
	}
	return u;
}

/*ngx_chain_t * ngx_http_clojure_socket_upstream_init_free_bufs(ngx_http_clojure_socket_upstream_t *u) {
	ngx_bufs_t bfs;
	bfs.num = u->buffer_num;
	bfs.size = u->buffer_size;
	return ngx_create_chain_of_bufs(u->pool, &bfs);
}*/

static void ngx_http_clojure_socket_upstream_handler(ngx_event_t *ev) {
	ngx_connection_t *c;
	ngx_http_clojure_socket_upstream_t *u;

	c = ev->data;
	u = c->data;

	if (!u->connect_event_sent) {
		if (ev->timedout) {
			ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_CONNECT_TIMEOUT);
			return;
		}

		if (c->write->timer_set) {
			ngx_del_timer(c->write);
		}

		if (ngx_http_clojure_socket_upstream_test_connect(c) == NGX_OK) {
			ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_OK);
		}else {
			ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_CONNECT);
			return;
		}
	}

	if (ev->write) {
		ngx_http_clojure_socket_upstream_write_handler(ev);
	} else {
		ngx_http_clojure_socket_upstream_read_handler(ev);
	}

}

static void ngx_http_clojure_socket_upstream_empty_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc/*status code*/) {
}

static void ngx_http_clojure_socket_upstream_connect_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc) {
	if (!u->connect_event_sent) {
		u->connect_event_handler(u, sc);
		u->connect_event_sent = 1;

		if (sc != NGX_HTTP_CLOJURE_SOCKET_OK) {
			ngx_http_clojure_socket_upstream_finalize(u, NGX_HTTP_CLOJURE_SOCKET_ERR_CONNECT);
		}
	}

}

static void ngx_http_clojure_socket_upstream_read_handler(ngx_event_t *ev) {
	ngx_connection_t *c;
	ngx_http_clojure_socket_upstream_t *u;

	c = ev->data;
	u = c->data;

	if (ev->timedout) {
		u->read_event_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_READ_TIMEOUT);
		return;
	}

	if (c->read->timer_set) {
		ngx_del_timer(c->read);
	}

	u->read_event_handler(u, NGX_HTTP_CLOJURE_SOCKET_OK);

}

static void ngx_http_clojure_socket_upstream_write_handler(ngx_event_t *ev) {
	ngx_connection_t *c;
	ngx_http_clojure_socket_upstream_t *u;

	c = ev->data;
	u = c->data;

	if (ev->timedout) {
		u->write_event_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_WRITE_TIMEOUT);
		return;
	}

	if (c->write->timer_set) {
		ngx_del_timer(c->write);
	}

	u->write_event_handler(u, NGX_HTTP_CLOJURE_SOCKET_OK);

}

void ngx_http_clojure_socket_upstream_connect_by_url(ngx_http_clojure_socket_upstream_t *u, ngx_url_t *url) {
	//TODO:  host name resolve by event driven
	if (url->addrs == NULL) {
		if (ngx_parse_url(u->pool, url) != NGX_OK ) {
			ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_RESOLVE);
			return;
		}
	}

	ngx_http_clojure_socket_upstream_connect(u, (struct sockaddr *)url->sockaddr, url->socklen);
}

static void ngx_http_clojure_socket_upstream_connect_1(ngx_http_clojure_socket_upstream_t *u) {
	ngx_int_t rc;
	ngx_connection_t  *c;
	ngx_peer_connection_t *pc = &u->peer;

/*	if (!u->free_bufs) {
		if (!ngx_http_clojure_socket_upstream_make_unset_to_default(u)) {
			ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_OUTOFMEMORY);
			return;
		}
	}*/

	pc->sockaddr = u->resolved->sockaddr;
	pc->socklen = u->resolved->socklen;
	/*TODO:keepalive peer*/
	pc->get = ngx_event_get_peer; /*do nothing!*/
	pc->free = NULL;
	pc->log = u->pool->log;
	pc->rcvbuf = u->buffer_size ? u->buffer_size : ngx_pagesize;
	pc->data = u;

	u->connect_event_sent = 0;
	rc = ngx_event_connect_peer(pc);

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, u->pool->log, 0,
	                   "clojure_socket_upstream connect: %i", rc);

	if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
		ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_ERR_CONNECT);
		return;
	}

	c = u->peer.connection;
	c->data = u;

	c->write->handler = c->read->handler = ngx_http_clojure_socket_upstream_handler;


	if (rc == NGX_AGAIN) {
		ngx_add_timer(c->write, u->connect_timeout ? u->connect_timeout : 30000);
		return;
	}

	/*connected immediately successfully*/
	ngx_http_clojure_socket_upstream_connect_handler(u, NGX_HTTP_CLOJURE_SOCKET_OK);
}

void ngx_http_clojure_socket_upstream_connect(ngx_http_clojure_socket_upstream_t *u, struct sockaddr *addr, socklen_t len) {
	u->resolved->sockaddr = addr;
	u->resolved->socklen = len;
	u->resolved->naddrs = 1;
	ngx_http_clojure_socket_upstream_connect_1(u);
}

int ngx_http_clojure_socket_upstream_read(ngx_http_clojure_socket_upstream_t *u, void *buf, size_t size) {
	ngx_connection_t  *c = u->peer.connection;
	ngx_int_t rc = ngx_recv(c, buf, size);
	if (rc == NGX_AGAIN) {
		if (u->read_timeout > 0) {
			ngx_add_timer(c->read, u->read_timeout);
		}
		rc = NGX_HTTP_CLOJURE_SOCKET_ERR_AGAIN;
	}else if (rc < 0) {
		rc = NGX_HTTP_CLOJURE_SOCKET_ERR_READ;
	}
	return rc;
}

int ngx_http_clojure_socket_upstream_write(ngx_http_clojure_socket_upstream_t *u, void *buf, size_t size) {
	ngx_connection_t  *c = u->peer.connection;
	ngx_int_t rc = ngx_send(c, buf, size);
	if (rc == 0 || rc == NGX_AGAIN) {
		if (u->read_timeout > 0) {
			ngx_add_timer(c->write, u->read_timeout);
		}
		rc = NGX_HTTP_CLOJURE_SOCKET_ERR_AGAIN;
	}else if (rc < 0) {
		rc = NGX_HTTP_CLOJURE_SOCKET_ERR_WRITE;
	}
	return rc;
}

void ngx_http_clojure_socket_upstream_close(ngx_http_clojure_socket_upstream_t *u) {
	ngx_http_clojure_socket_upstream_finalize(u, NGX_HTTP_CLOJURE_SOCKET_OK);
}

int ngx_http_clojure_socket_upstream_shutdown(ngx_http_clojure_socket_upstream_t *u, int how) {
	if (how & NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_READ) {
		u->read_event_handler = ngx_http_clojure_socket_upstream_empty_handler;
	}else if (how & NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_WRITE) {
		u->write_event_handler = ngx_http_clojure_socket_upstream_empty_handler;
	}else if (how & NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_BOTH) {
		u->read_event_handler = ngx_http_clojure_socket_upstream_empty_handler;
		u->write_event_handler = ngx_http_clojure_socket_upstream_empty_handler;
	}else {
		return NGX_HTTP_CLOJURE_SOCKET_ERR;
	}
	if (how & NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_SOFT_FLAG) {
		return NGX_HTTP_CLOJURE_SOCKET_OK;
	}
	if (ngx_shutdown_socket(u->peer.connection->fd, how) != 0) {
		return NGX_HTTP_CLOJURE_SOCKET_ERR;
	}
	return NGX_HTTP_CLOJURE_SOCKET_OK;
}

static void JNICALL jni_ngx_http_clojure_socket_read_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc) {
	(*jvm_env)->CallVoidMethod(jvm_env, (jobject)u->context, nc_socket_handler_read_mid, (uintptr_t)u, (jlong)sc);
	exception_handle(0 == 0, jvm_env, return);
}

static void JNICALL jni_ngx_http_clojure_socket_write_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc) {
	(*jvm_env)->CallVoidMethod(jvm_env, (jobject)u->context, nc_socket_handler_write_mid, (uintptr_t)u, (jlong)sc);
	exception_handle(0 == 0, jvm_env, return);
}

static void JNICALL jni_ngx_http_clojure_socket_connect_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc) {
	(*jvm_env)->CallVoidMethod(jvm_env, (jobject)u->context, nc_socket_handler_connect_mid, (uintptr_t)u, (jlong)sc);
	exception_handle(0 == 0, jvm_env, return);
}

static void JNICALL jni_ngx_http_clojure_socket_release_handler(ngx_http_clojure_socket_upstream_t *u, ngx_int_t sc) {
	(*jvm_env)->CallVoidMethod(jvm_env, (jobject)u->context, nc_socket_handler_release_mid, (uintptr_t)u, (jlong)sc);
	exception_handle(0 == 0, jvm_env, (*jvm_env)->DeleteGlobalRef(jvm_env, (jobject)u->context));
	(*jvm_env)->DeleteGlobalRef(jvm_env, (jobject)u->context);
}

static jlong JNICALL jni_ngx_http_clojure_socket_create(JNIEnv *env, jclass cls, jobject handler) {
	ngx_http_clojure_socket_upstream_t *u = ngx_http_clojure_socket_upstream_create(4096, ngx_http_clojure_global_cycle->log);
	ngx_http_clojure_socket_upstream_set_context(u, handler);
	ngx_http_clojure_socket_upstream_set_event_handler(u,
			jni_ngx_http_clojure_socket_read_handler,
			jni_ngx_http_clojure_socket_write_handler,
			jni_ngx_http_clojure_socket_connect_handler,
			jni_ngx_http_clojure_socket_release_handler);
	jobject gh = (*env)->NewGlobalRef(env, handler);
	exception_handle(gh == NULL, env, return 0);
	ngx_http_clojure_socket_upstream_set_context(u, gh);
	return (uintptr_t)u;
}

static jlong JNICALL jni_ngx_http_clojure_socket_connect_url(JNIEnv *env, jclass cls, jlong s, jobject jurl, jlong off, jlong len) {
	ngx_http_clojure_socket_upstream_t *u = (ngx_http_clojure_socket_upstream_t *)s;
	ngx_url_t *url = ngx_pcalloc(u->pool, sizeof(ngx_url_t));
	if (url == NULL){
		return NGX_HTTP_CLOJURE_SOCKET_ERR_OUTOFMEMORY;
	}
	url->url.data = ngx_pcalloc(u->pool, (size_t)len);
	if (url->url.data == NULL){
			return NGX_HTTP_CLOJURE_SOCKET_ERR_OUTOFMEMORY;
	}
	ngx_memcpy(url->url.data, (char *)(*(uintptr_t*)jurl) + off, (size_t)len);
	url->url.len = (size_t)len;
	ngx_http_clojure_socket_upstream_connect_by_url(u, url);
	return NGX_HTTP_CLOJURE_SOCKET_OK;
}

static void jni_ngx_http_clojure_socket_set_timeout(JNIEnv *env, jclass cls, jlong s, jlong ctimeout, jlong rtimeout, jlong wtimeout) {
	ngx_http_clojure_socket_upstream_t *u = (ngx_http_clojure_socket_upstream_t *)s;
	if (ctimeout >= 0) {
		ngx_http_clojure_socket_upstream_set_connect_timeout(u, ctimeout);
	}
	if (rtimeout >= 0) {
		ngx_http_clojure_socket_upstream_set_read_timeout(u, rtimeout);
	}
	if (wtimeout >= 0) {
		ngx_http_clojure_socket_upstream_set_write_timeout(u, wtimeout);
	}
}

static jlong jni_ngx_http_clojure_socket_get_connect_timeout(JNIEnv *env, jclass cls, jlong s) {
	return ((ngx_http_clojure_socket_upstream_t *)s)->connect_timeout;
}

static jlong jni_ngx_http_clojure_socket_get_read_timeout(JNIEnv *env, jclass cls, jlong s) {
	return ((ngx_http_clojure_socket_upstream_t *)s)->read_timeout;
}

static jlong jni_ngx_http_clojure_socket_get_write_timeout(JNIEnv *env, jclass cls, jlong s) {
	return ((ngx_http_clojure_socket_upstream_t *)s)->write_timeout;
}

static void jni_ngx_http_clojure_socket_set_receive_buf(JNIEnv *env, jclass cls, jlong s, jlong size) {
	((ngx_http_clojure_socket_upstream_t *)s)->buffer_size = (size_t)size;
}

static jlong jni_ngx_http_clojure_socket_get_receive_buf(JNIEnv *env, jclass cls, jlong s) {
	return (jlong)((ngx_http_clojure_socket_upstream_t *)s)->buffer_size;
}

static jlong JNICALL jni_ngx_http_clojure_socket_read(JNIEnv *env, jclass cls, jlong s, jobject buf, jlong off, jlong len) {
	char *dst = (char *)(*(uintptr_t*)buf) + off;
	return ngx_http_clojure_socket_upstream_read((ngx_http_clojure_socket_upstream_t *)s, dst, len);
}

static jlong JNICALL jni_ngx_http_clojure_socket_write(JNIEnv *env, jclass cls, jlong s, jobject buf, jlong off, jlong len) {
	char *src = (char *)(*(uintptr_t*)buf) + off;
	return ngx_http_clojure_socket_upstream_write((ngx_http_clojure_socket_upstream_t *)s, src, len);
}

static void JNICALL jni_ngx_http_clojure_socket_close(JNIEnv *env, jclass cls, jlong u) {
	ngx_http_clojure_socket_upstream_close((ngx_http_clojure_socket_upstream_t *)u);
}

static jlong JNICALL jni_ngx_http_clojure_socket_shutdown(JNIEnv *env, jclass cls, jlong s, jlong how) {
	return ngx_http_clojure_socket_upstream_shutdown((ngx_http_clojure_socket_upstream_t *)s, (int)how);
}

static jlong JNICALL jni_ngx_http_clojure_socket_cancel_soft_shutdown(JNIEnv *env, jclass cls, jlong s, jlong how) {
	ngx_http_clojure_socket_upstream_t *u = (ngx_http_clojure_socket_upstream_t *)s;
	if (!(how & NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_SOFT_FLAG)) {
		return NGX_HTTP_CLOJURE_SOCKET_ERR;
	}
	if (how == NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_SOFT_READ) {
		u->read_event_handler = jni_ngx_http_clojure_socket_read_handler;
	} else if (how == NGX_HTTP_CLOJURE_SOCKET_SHUTDOWN_SOFT_WRITE) {
		u->write_event_handler = jni_ngx_http_clojure_socket_write_handler;
	} else {
		u->read_event_handler = jni_ngx_http_clojure_socket_read_handler;
		u->write_event_handler = jni_ngx_http_clojure_socket_write_handler;
	}
	return NGX_HTTP_CLOJURE_SOCKET_OK;
}

int ngx_http_clojure_init_socket_util() {
	JNIEnv *env;
	JNINativeMethod nms[] = {
			{"create", "(Lnginx/clojure/net/NginxClojureSocketRawHandler;)J", jni_ngx_http_clojure_socket_create},
			{"setTimeout", "(JJJJ)V", jni_ngx_http_clojure_socket_set_timeout},
			{"getReadTimeout", "(J)J", jni_ngx_http_clojure_socket_get_read_timeout},
			{"getWriteTimeout", "(J)J", jni_ngx_http_clojure_socket_get_write_timeout},
			{"getConnectTimeout", "(J)J", jni_ngx_http_clojure_socket_get_connect_timeout},
			{"getReceiveBufferSize", "(J)J", jni_ngx_http_clojure_socket_set_receive_buf},
			{"setReceiveBufferSize", "(JJ)J", jni_ngx_http_clojure_socket_get_receive_buf},
			{"connect", "(JLjava/lang/Object;JJ)J", jni_ngx_http_clojure_socket_connect_url},
			{"read", "(JLjava/lang/Object;JJ)J", jni_ngx_http_clojure_socket_read},
			{"write", "(JLjava/lang/Object;JJ)J", jni_ngx_http_clojure_socket_write},
			{"close", "(J)V", jni_ngx_http_clojure_socket_close},
			{"shutdown", "(JJ)J", jni_ngx_http_clojure_socket_shutdown},
			{"cancelSoftShutdown", "(JJ)J", jni_ngx_http_clojure_socket_cancel_soft_shutdown}
	};
	jclass nc_socket_handler_class;

	if (ngx_http_clojure_init_socket_flag != NGX_HTTP_CLOJURE_JVM_ERR) {
		return NGX_HTTP_CLOJURE_JVM_OK;
	}

	ngx_http_clojure_get_env(&jvm_env);
	env = jvm_env;
	nc_socket_class = (*jvm_env)->FindClass(env, "nginx/clojure/net/NginxClojureAsynSocket");
	exception_handle(nc_socket_class == NULL, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	nc_socket_handler_class = (*jvm_env)->FindClass(env, "nginx/clojure/net/NginxClojureSocketRawHandler");
	exception_handle(nc_socket_handler_class == NULL, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	nc_socket_handler_read_mid  = (*env)->GetMethodID(env, nc_socket_handler_class,"onRead", "(JJ)V");
	exception_handle(nc_socket_handler_read_mid == NULL, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	nc_socket_handler_write_mid  = (*env)->GetMethodID(env, nc_socket_handler_class,"onWrite", "(JJ)V");
	exception_handle(nc_socket_handler_write_mid == NULL, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	nc_socket_handler_connect_mid  = (*env)->GetMethodID(env, nc_socket_handler_class,"onConnect", "(JJ)V");
	exception_handle(nc_socket_handler_connect_mid == NULL, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	nc_socket_handler_release_mid  = (*env)->GetMethodID(env, nc_socket_handler_class,"onRelease", "(JJ)V");
	exception_handle(nc_socket_handler_release_mid == NULL, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	(*env)->RegisterNatives(env, nc_socket_class, nms, sizeof(nms) / sizeof(JNINativeMethod));
	exception_handle(0 == 0, env, return NGX_HTTP_CLOJURE_JVM_ERR);

	return NGX_HTTP_CLOJURE_JVM_OK;
}
