// janus_transport_wsclient.c
#include "transport.h"
#include <glib.h>
#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>

#define JANUS_WSCLIENT_NAME				"JANUS WebSockets client transport plugin"
#define KEEPALIVE_INTERVAL 30  // seconds
/* JSON serialization options */
static size_t json_format = JSON_INDENT(0) | JSON_PRESERVE_ORDER;
//static size_t json_format = JSON_INDENT(3) | JSON_PRESERVE_ORDER;

typedef struct janus_wsclient_session {
    GThread *service_thread;
    GThread *keepalive_thread;
    struct lws *ws_client_wsi;
    struct lws_context *ws_context;
    guint64 session_id;
    gboolean running;
    gboolean connected;
    void *user_data;    // user data
    char *incoming;							/* Buffer containing the incoming message to process (in case there are fragments) */
} janus_wsclient_session;

static janus_wsclient_transport_callbacks *gateway = NULL;
janus_wsclient_transport *wsclient_create(janus_wsclient_transport_callbacks *callback);
void *janus_wsclient_init(char *instance_ip, guint16 instance_port, void *user_data);;
void janus_wsclient_destroy(void *transport);
int janus_wsclient_send_message(void *transport, json_t *msg, gboolean is_text);

/* Transport setup */
static janus_wsclient_transport janus_wsclient =
    JANUS_WSCLIENT_TRANSPORT_INIT (
        .init = janus_wsclient_init,
        .destroy = janus_wsclient_destroy,
        .send_message = janus_wsclient_send_message,
    );


/* Transport creator */
janus_wsclient_transport *wsclient_create(janus_wsclient_transport_callbacks *callback) {
    JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_WSCLIENT_NAME);
    if (callback == NULL) return NULL;

    gateway = callback;

    return &janus_wsclient;
}

static int callback_wsclient(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
    janus_wsclient_session *session = (janus_wsclient_session *)lws_context_user(lws_get_context(wsi));
   	const char *log_prefix = "WSClient";

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            JANUS_LOG(LOG_INFO, "WebSocket client connected.\n");
            session->connected = TRUE;
        //  lws_callback_on_writable(wsi);
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            JANUS_LOG(LOG_HUGE, "[%s-%p] Got %zu bytes:\n", log_prefix, session->ws_client_wsi, len);
			if(session == NULL || session->ws_client_wsi == NULL) {
				JANUS_LOG(LOG_ERR, "[%s-%p] Invalid WebSocket client instance...\n", log_prefix, wsi);
				return -1;
			}
			// if(g_atomic_int_get(&session->destroyed))
			// 	return 0;
#if (LWS_LIBRARY_VERSION_MAJOR >= 4)
			/* Refresh the lws connection validity (avoid sending a ping) */
			lws_validity_confirmed(session->wsi);
#endif
			/* Is this a new message, or part of a fragmented one? */
			const size_t remaining = lws_remaining_packet_payload(wsi);
			if(session->incoming == NULL) {
				JANUS_LOG(LOG_HUGE, "[%s-%p] First fragment: %zu bytes, %zu remaining\n", log_prefix, wsi, len, remaining);
				session->incoming = g_malloc(len+1);
				memcpy(session->incoming, in, len);
				session->incoming[len] = '\0';
				JANUS_LOG(LOG_INFO, "WebSocket message received: %s\n", session->incoming);
			} else {
				size_t offset = strlen(session->incoming);
				JANUS_LOG(LOG_HUGE, "[%s-%p] Appending fragment: offset %zu, %zu bytes, %zu remaining\n", log_prefix, wsi, offset, len, remaining);
				session->incoming = g_realloc(session->incoming, offset+len+1);
				memcpy(session->incoming+offset, in, len);
				session->incoming[offset+len] = '\0';
				JANUS_LOG(LOG_HUGE, "%s\n", session->incoming+offset);
			}
			if(remaining > 0 || !lws_is_final_fragment(wsi)) {
				/* Still waiting for some more fragments */
				JANUS_LOG(LOG_HUGE, "[%s-%p] Waiting for more fragments\n", log_prefix, wsi);
				return 0;
			}
			JANUS_LOG(LOG_HUGE, "[%s-%p] Done, parsing message: %zu bytes\n", log_prefix, wsi, strlen(session->incoming));
			
			///////
            if (gateway && gateway->push_conf_events) {
                gateway->push_conf_events(session->incoming, len, (void *)session->user_data);
                //gateway->push_conf_events((char *)in, len, (void *)session->user_data);

            }
            g_free(session->incoming);
			session->incoming = NULL;
            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            JANUS_LOG(LOG_ERR, "WebSocket connection error: %s\n",
            in ? (char *)in : "Unknown error");
            session->connected = FALSE;
            break;
        case LWS_CALLBACK_CLIENT_CLOSED:
            JANUS_LOG(LOG_INFO, "WebSocket client disconnected.\n");
            session->connected = FALSE;
            break;
        default:
            break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    {
        "janus-protocol",
        callback_wsclient,
        sizeof(janus_wsclient_session),
        4096,
        0,
        NULL,
        0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0}
};

static gpointer service_loop(gpointer data) {
    janus_wsclient_session *session = (janus_wsclient_session *)data;
        JANUS_LOG(LOG_INFO, "Established WebSocket context. Starting service loop.\n");
    
    while (session->running && session->ws_context != NULL) {
        lws_service(session->ws_context, 50);
    }
    JANUS_LOG(LOG_INFO, "Established WebSocket context. Exiting service loop.\n");
    return NULL;
}

static char *janus_transport_random_uuid(void) {
#if GLIB_CHECK_VERSION(2, 52, 0)
    return g_uuid_string_random();
#else
    /* g_uuid_string_random is only available from glib 2.52, so if it's
    * not available we have to do it manually: the following code is
    * heavily based on https://github.com/rxi/uuid4 (MIT license) */
    const char *template = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    const char *samples = "0123456789abcdef";
    union { unsigned char b[16]; uint64_t word[2]; } rnd;
    rnd.word[0] = janus_random_uint64();
    rnd.word[1] = janus_random_uint64();
    /* Generate the string */
    char uuid[37], *dst = uuid;
    const char *p = template;
    int i = 0, n = 0;
    while(*p) {
        n = rnd.b[i >> 1];
        n = (i & 1) ? (n >> 4) : (n & 0xf);
        switch (*p) {
            case 'x':
                *dst = samples[n];
                i++;
                break;
            case 'y':
                *dst = samples[(n & 0x3) + 8];
                i++;
                break;
            default:
                *dst = *p;
        }
        p++;
        dst++;
    }
    uuid[36] = '\0';
    return g_strdup(uuid);
#endif
}

static gpointer keepalive_loop(gpointer data) {
    janus_wsclient_session *session = data;
    gint64 start_time = g_get_real_time(); // microseconds
    gint64 target_time = start_time + (2 * G_USEC_PER_SEC);

    while(session->connected == FALSE) {
        gint64 now = g_get_real_time();

        if (now >= target_time) {
            break;
        }
        g_usleep(10000); // Sleep for 10ms
    }

    while (session->running) {
        if (session->ws_client_wsi && session->session_id) {
            json_t *root = json_object();
            json_object_set_new(root, "janus", json_string("keepalive"));
            
            char *transaction_string = janus_transport_random_uuid();
            json_object_set_new(root, "transaction", json_string(transaction_string));
            json_object_set_new(root, "session_id", json_integer(session->session_id));

            char *payload = json_dumps(root, json_format);
            if (!payload) {
                JANUS_LOG(LOG_ERR, "Invalid JSON message\n");
                return NULL;
            }
            JANUS_LOG(LOG_INFO, "Websocket message send: %s\n",payload);

            size_t len = strlen(payload);
            unsigned char *buf = g_malloc(LWS_PRE + len);
            //unsigned char *buf = g_malloc(LWS_PRE + msg_len + LWS_POST);
            unsigned char *p = buf + LWS_PRE;
            memcpy(p, payload, len);

            lws_write(session->ws_client_wsi, p, len, LWS_WRITE_TEXT);
                            
            // ret = lws_write(session->ws_client_wsi, p, len,
            //                 is_text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY);
            g_free(buf);
            g_free(payload);
            g_free(transaction_string);
            json_decref(root);
        } 

        for (int i = 0; i < 300; i++) {
            if (session->running == FALSE) break;
            g_usleep(100000); // Sleep for 100ms
        }
    }
   	JANUS_LOG(LOG_INFO, "TEST: keepalive loop exit\n");
    return NULL;
}

void *janus_wsclient_init(char *instance_ip, guint16 instance_port, void *user_data) {
    janus_wsclient_session *session = g_malloc0(sizeof(janus_wsclient_session));
    struct lws_context_creation_info info;
    struct lws_client_connect_info ccinfo;

    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.user = (void *)session;

    session->user_data = (void *)user_data;
    session->session_id = 0;
    session->running = TRUE;
    session->connected = FALSE;

    session->ws_context = lws_create_context(&info);
    if (!session->ws_context) {
        JANUS_LOG(LOG_FATAL, "Failed to create WebSocket context.\n");
        g_free(session);
        return NULL;
    }

    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = session->ws_context;
    ccinfo.address = instance_ip;
    ccinfo.port = instance_port;
    ccinfo.path = "/janus";
    // ccinfo.host = "janus";
    // ccinfo.origin = "origin";
    ccinfo.host = lws_canonical_hostname(session->ws_context);
    ccinfo.origin = "origin";
    ccinfo.protocol = "janus-protocol";
    ccinfo.ssl_connection = 0;
    
    JANUS_LOG(LOG_INFO, "Established WebSocket context. %s\n",lws_canonical_hostname(session->ws_context));
    JANUS_LOG(LOG_INFO, "instance_ip: %s.\n", instance_ip);
    JANUS_LOG(LOG_INFO, "instance_port %d.\n", instance_port);
    session->ws_client_wsi = lws_client_connect_via_info(&ccinfo);

    if (!session->ws_client_wsi) {
        JANUS_LOG(LOG_FATAL, "WebSocket client connection failed.\n");
        lws_context_destroy(session->ws_context);
        g_free(session);
        return NULL;
    }

    session->service_thread = g_thread_new("wsclient-service", service_loop, session);
    session->keepalive_thread = g_thread_new("wsclient-keepalive", keepalive_loop, session);
    gint64 start_time = g_get_real_time(); // microseconds
    gint64 target_time = start_time + (2 * G_USEC_PER_SEC);

    while(session->connected == FALSE) {
        gint64 now = g_get_real_time();

        if (now >= target_time) {
            janus_wsclient_destroy(session);
            JANUS_LOG(LOG_INFO, "Unable to establish webSocket connection within 2 seconds.\n");
            return NULL;
        }
        g_usleep(10000); // Sleep for 10ms
    }
    JANUS_LOG(LOG_INFO, "Established webSocket connection.\n");

    return session;
}

void janus_wsclient_destroy(void *transport) {
    if (!transport) return;
    janus_wsclient_session *session = (janus_wsclient_session *)transport;
    session->running = FALSE;
	JANUS_LOG(LOG_INFO, "TEST1: janus_wsclient_destroy\n");

    if (session->service_thread)
        g_thread_join(session->service_thread);
   	JANUS_LOG(LOG_INFO, "TEST2: janus_wsclient_destroy\n");

    if (session->keepalive_thread)
        g_thread_join(session->keepalive_thread);
   	JANUS_LOG(LOG_INFO, "TEST3: janus_wsclient_destroy\n");

    if (session->ws_context)
        lws_context_destroy(session->ws_context);
   	JANUS_LOG(LOG_INFO, "TEST5: janus_wsclient_destroy\n");

    g_free(session);
}

int janus_wsclient_send_message(void *transport, json_t *msg, gboolean is_text) {
    int ret = -1;
    if (!transport || !msg) return ret;
    janus_wsclient_session *session = transport;

    if (!session->ws_client_wsi) return ret;

    if (!session->connected) return ret;

    if (session->session_id == 0) {
        json_t *message = json_object_get(msg, "janus");
        const gchar *message_text = json_string_value(message);
        if (!strcasecmp(message_text, "attach")) {
            session->session_id = json_integer_value(json_object_get(msg, "session_id"));
        }
    }
    char *payload = json_dumps(msg, json_format);
    if (!payload) {
        JANUS_LOG(LOG_ERR, "Invalid JSON message\n");
        return ret;
    }
    JANUS_LOG(LOG_INFO, "\tWS message send...%s\n",payload);

    size_t len = strlen(payload);
    unsigned char *buf = g_malloc(LWS_PRE + len);
    //unsigned char *buf = g_malloc(LWS_PRE + msg_len + LWS_POST);
    unsigned char *p = buf + LWS_PRE;
    memcpy(p, payload, len);

    ret = lws_write(session->ws_client_wsi, p, len, LWS_WRITE_TEXT);
                    
    // ret = lws_write(session->ws_client_wsi, p, len,
    //                 is_text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY);
    g_free(buf);
    p = NULL;
    g_free(payload);
    return ret;
}