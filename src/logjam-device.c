#include <zmq.h>
#include <czmq.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <getopt.h>
#include <amqp_tcp_socket.h>
#include <amqp.h>
#include <amqp_framing.h>


void log_error(int x, char const *context) {
    if (x < 0) {
        char const *errstr = amqp_error_string2(-x);
        fprintf(stderr, "%s: %s\n", context, errstr);
    }
}

void die_on_error(int x, char const *context) {
    if (x < 0) {
        log_error(x, context);
        exit(1);
    }
}

int log_amqp_error(amqp_rpc_reply_t x, char const *context) {
    switch (x.reply_type) {
    case AMQP_RESPONSE_NORMAL:
        return 0;

    case AMQP_RESPONSE_NONE:
        fprintf(stderr, "%s: missing RPC reply type!\n", context);
        break;

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
        fprintf(stderr, "%s: %s\n", context, amqp_error_string2(x.library_error));
        break;

    case AMQP_RESPONSE_SERVER_EXCEPTION:
        switch (x.reply.id) {
	case AMQP_CONNECTION_CLOSE_METHOD: {
            amqp_connection_close_t *m = (amqp_connection_close_t *) x.reply.decoded;
            fprintf(stderr, "%s: server connection error %d, message: %.*s\n",
                    context,
                    m->reply_code,
                    (int) m->reply_text.len, (char *) m->reply_text.bytes);
            break;
	}
	case AMQP_CHANNEL_CLOSE_METHOD: {
            amqp_channel_close_t *m = (amqp_channel_close_t *) x.reply.decoded;
            fprintf(stderr, "%s: server channel error %d, message: %.*s\n",
                    context,
                    m->reply_code,
                    (int) m->reply_text.len, (char *) m->reply_text.bytes);
            break;
	}
	default:
            fprintf(stderr, "%s: unknown server error, method id 0x%08X\n", context, x.reply.id);
            break;
        }
        break;
    }

    return 1;
}

void die_on_amqp_error(amqp_rpc_reply_t x, char const *context)
{
    if (0 != log_amqp_error(x, context)) {
        exit(1);
    }
}

void assert_x(int rc, const char* error_text)
{
    if (rc != 0) {
        printf("Failed assertion: %s\n", error_text);
        exit(1);
    }
}

void log_zmq_error(int rc)
{
    if (rc != 0) {
        printf("errno: %d: %s\n", errno, zmq_strerror(errno));
    }
}

void assert_zmq_rc(int rc)
{
    if (rc != 0) {
        printf("rc: %d: %s\n", errno, zmq_strerror(errno));
        assert(rc == 0);
    }
}

/* global config */
static zconfig_t* config = NULL;
char *config_file = "logjam.conf";

static char* rabbit_host = "localhost";
static int rabbit_port = 5672;
static int pull_port = 9605;
static int pub_port = 9606;

static unsigned long received_messages_count = 0;
// hash of already declared exchanges
static zhash_t *exchanges = NULL;

#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
#define FIX_SIG_PIPE
#endif

amqp_connection_state_t setup_amqp_connection()
{
    amqp_connection_state_t connection = amqp_new_connection();
    amqp_socket_t *socket = amqp_tcp_socket_new(connection);

#ifdef FIX_SIG_PIPE
    // why doesn't librabbitmq do this, eh?
    int sockfd = amqp_get_sockfd(connection);
    int one = 1; //
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    int status = amqp_socket_open(socket, rabbit_host, rabbit_port);
    assert_x(status, "Opening RabbitMQ socket");

    die_on_amqp_error(amqp_login(connection, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest"),
                      "Logging in to RabbitMQ");
    amqp_channel_open(connection, 1);
    die_on_amqp_error(amqp_get_rpc_reply(connection), "Opening AMQP channel");

    return connection;
}

int publish_on_zmq_transport(zmq_msg_t *message_parts, void *publisher)
{
    int rc=0;
    zmq_msg_t *exchange  = &message_parts[0];
    zmq_msg_t *key       = &message_parts[1];
    zmq_msg_t *body      = &message_parts[2];

    rc = zmq_msg_send(exchange, publisher, ZMQ_SNDMORE|ZMQ_DONTWAIT);
    if (rc == -1) {
        log_zmq_error(rc);
        return rc;
    }
    rc = zmq_msg_send(key, publisher, ZMQ_SNDMORE|ZMQ_DONTWAIT);
    if (rc == -1) {
        log_zmq_error(rc);
        return rc;
    }
    rc = zmq_msg_send(body, publisher, ZMQ_DONTWAIT);
    if (rc == -1) {
        log_zmq_error(rc);
        return rc;
    }
    return rc;
}

int publish_on_amqp_transport(amqp_connection_state_t connection, zmq_msg_t *message_parts)
{
    zmq_msg_t *exchange  = &message_parts[0];
    zmq_msg_t *key       = &message_parts[1];
    zmq_msg_t *body      = &message_parts[2];

    // extract data
    amqp_bytes_t exchange_name;
    exchange_name.len   = zmq_msg_size(exchange);
    exchange_name.bytes = zmq_msg_data(exchange);

    char *exchange_string = malloc(exchange_name.len + 1);
    memcpy(exchange_string, exchange_name.bytes, exchange_name.len);
    exchange_string[exchange_name.len] = 0;

    int found = zhash_insert(exchanges, exchange_string, NULL);
    if (-1 == found) {
        free(exchange_string);
    } else {
        // declare exchange, as we haven't seen it before
        amqp_boolean_t passive = 0;
        amqp_boolean_t durable = 1;
        amqp_bytes_t exchange_type;
        exchange_type = amqp_cstring_bytes("topic");
        amqp_exchange_declare(connection, 1, exchange_name, exchange_type, passive, durable, amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(connection), "Declaring AMQP exchange");
    }

    amqp_bytes_t routing_key;
    routing_key.len   = zmq_msg_size(key);
    routing_key.bytes = zmq_msg_data(key);

    amqp_bytes_t message_body;
    message_body.len   = zmq_msg_size(body);
    message_body.bytes = zmq_msg_data(body);

    //printf("publishing\n");

    // send message to rabbit
    int amqp_rc = amqp_basic_publish(connection,
                                     1,
                                     exchange_name,
                                     routing_key,
                                     0,
                                     0,
                                     NULL,
                                     message_body);
    return amqp_rc;
}


void shutdown_amqp_connection(amqp_connection_state_t connection, int amqp_rc)
{
    // close amqp connection
    if (amqp_rc >= 0) {
        log_amqp_error(amqp_channel_close(connection, 1, AMQP_REPLY_SUCCESS), "Closing AMQP channel");
        log_amqp_error(amqp_connection_close(connection, AMQP_REPLY_SUCCESS), "Closing AMQP connection");
    }
    log_error(amqp_destroy_connection(connection), "Ending AMQP connection");
}


int timer_event(zloop_t *loop, zmq_pollitem_t *item, void *arg)
{
    static unsigned long last_received_count = 0;
    printf("processed %lu messages.\n", received_messages_count-last_received_count);
    last_received_count = received_messages_count;
    return 0;
}

typedef struct {
    void *publisher;
    amqp_connection_state_t connection;
    int amqp_rc;
} publisher_state_t;

int read_zmq_message_and_forward(zloop_t *loop, zmq_pollitem_t *item, void *callback_data)
{
    int i = 0;
    zmq_msg_t message_parts[3];
    void *receiver = item->socket;
    publisher_state_t *state = (publisher_state_t*)callback_data;
    void *publisher = state->publisher;
    amqp_connection_state_t connection = state->connection;

    // read the message parts
    while (!zctx_interrupted) {
        // printf("receiving part %d\n", i+1);
        if (i>2) {
            printf("Received more than 3 message parts\n");
            exit(1);
        }
        zmq_msg_init(&message_parts[i]);
        zmq_recvmsg(receiver, &message_parts[i], 0);
        if (!zsocket_rcvmore(receiver))
            break;
        i++;
    }
    if (i<2) {
        if (!zctx_interrupted) {
            printf("Received only %d message parts\n", i);
        }
        goto cleanup;
    }
    received_messages_count++;

    if (connection != NULL) {
        state->amqp_rc = publish_on_amqp_transport(connection, &message_parts[0]);

        if (state->amqp_rc < 0) {
            log_error(state->amqp_rc, "Publishing via AMQP");
            zctx_interrupted = 1;
            goto cleanup;
        }
    }

    publish_on_zmq_transport(&message_parts[0], publisher);

 cleanup:
    for(;i>=0;i--) {
        zmq_msg_close(&message_parts[i]);
    }

    return 0;
}

void rabbitmq_add_queue(amqp_connection_state_t conn, const char *stream)
{
    const char *exchange = stream;
    const char *routing_key = "#";
    size_t n = strlen(stream);
    char app[n], env[n];
    memset(app, 0, n);
    memset(env, 0, n);
    sscanf(stream, "request-stream-%[^-]-%[^-]", app, env);
    char queue[n];
    sprintf(queue, "logjam-device-%s-%s", app, env);
    printf("binding: %s ==> %s\n", exchange, queue);

    // amqp_exchange_declare(amqp_connection_state_t state, amqp_channel_t channel, amqp_bytes_t exchange,
    //                       amqp_bytes_t type, amqp_boolean_t passive, amqp_boolean_t durable, amqp_table_t arguments);
    amqp_exchange_declare(conn, 1, amqp_cstring_bytes(exchange), amqp_cstring_bytes("topic"),
                          0, 1, amqp_empty_table);

    amqp_bytes_t queuename = amqp_cstring_bytes(queue);

    struct amqp_table_entry_t_ queue_arg_table_entries[1] = {
        { amqp_cstring_bytes("x-message-ttl"), { AMQP_FIELD_KIND_I32, { 30 * 1000 } } }
    };
    amqp_table_t queue_argument_table = {1, queue_arg_table_entries};

    // amqp_queue_declare(amqp_connection_state_t state, amqp_channel_t channel, amqp_bytes_t queue,
    //                    amqp_boolean_t passive, amqp_boolean_t durable, amqp_boolean_t exclusive, amqp_boolean_t auto_delete,
    //                    amqp_table_t arguments);
    amqp_queue_declare(conn, 1, queuename, 0, 0, 0, 1, queue_argument_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Declaring queue");

    // amqp_queue_bind(amqp_connection_state_t state, amqp_channel_t channel, amqp_bytes_t queue,
    //                 amqp_bytes_t exchange, amqp_bytes_t routing_key, amqp_table_t arguments)
    amqp_queue_bind(conn, 1, queuename, amqp_cstring_bytes(exchange), amqp_cstring_bytes(routing_key), amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Binding queue");

    // amqp_basic_consume(amqp_connection_state_t state, amqp_channel_t channel, amqp_bytes_t queue, amqp_bytes_t consumer_tag,
    // amqp_boolean_t no_local, amqp_boolean_t no_ack, amqp_boolean_t exclusive, amqp_table_t arguments)
    amqp_basic_consume(conn, 1, queuename, amqp_empty_bytes, 0, 1, 0, amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming");
}

void rabbitmq_listener(void *args, zctx_t *context, void *pipe) {
    amqp_connection_state_t conn = setup_amqp_connection();

    zconfig_t *streams = zconfig_locate(config, "backend/streams");
    assert(streams);
    zconfig_t *stream_config = zconfig_child(streams);
    assert(stream_config);
    do {
        char *stream = zconfig_name(stream_config);
        rabbitmq_add_queue(conn, stream);
        stream_config = zconfig_next(stream_config);
    } while (stream_config && !zctx_interrupted);

    while (!zctx_interrupted) {
        amqp_rpc_reply_t res;
        amqp_envelope_t envelope;

        amqp_maybe_release_buffers(conn);

        res = amqp_consume_message(conn, &envelope, NULL, 0);

        if (AMQP_RESPONSE_NORMAL != res.reply_type) {
            zctx_interrupted = 1;
            log_amqp_error(res, "Consuming from RabbitMQ");
            break;
        }

        if (0) {
            printf("Delivery %u, exchange %.*s routingkey %.*s\n",
                   (unsigned) envelope.delivery_tag,
                   (int) envelope.exchange.len, (char *) envelope.exchange.bytes,
                   (int) envelope.routing_key.len, (char *) envelope.routing_key.bytes);

            if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
                printf("Content-type: %.*s\n",
                   (int) envelope.message.properties.content_type.len,
                   (char *) envelope.message.properties.content_type.bytes);
            }
        }

        // send messages to main thread
        zmsg_t *message = zmsg_new();
        zmsg_addmem(message, envelope.exchange.bytes, envelope.exchange.len);
        zmsg_addmem(message, envelope.routing_key.bytes, envelope.routing_key.len);
        zmsg_addmem(message, envelope.message.body.bytes, envelope.message.body.len);
        // zmsg_dump(message);
        zmsg_send(&message, pipe);
        zmsg_destroy(&message);

        amqp_destroy_envelope(&envelope);
    }

    // 0 is not the return code
    shutdown_amqp_connection(conn, 0);
}

void print_usage(char * const *argv)
{
    fprintf(stderr, "usage: %s [-r rabbit-host] [-p pull-port] [-c config-file]\n", argv[0]);
}

void process_arguments(int argc, char * const *argv)
{
    char c;
    opterr = 0;
    while ((c = getopt(argc, argv, "r:p:c:")) != -1) {
        switch (c) {
        case 'r':
            rabbit_host = optarg;
            break;
        case 'p':
            pull_port = atoi(optarg);
            break;
        case 'c':
            config_file = optarg;
            break;
        case '?':
            if (optopt == 'c' || optopt == 'p' || optopt == 'r')
                fprintf(stderr, "option -%c requires an argument.\n", optopt);
            else if (isprint (optopt))
                fprintf(stderr, "unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "unknown option character `\\x%x'.\n", optopt);
            print_usage(argv);
            exit(1);
        default:
            exit(1);
        }
    }
}

int main(int argc, char * const *argv)
{
    int rc=0;
    process_arguments(argc, argv);

    if (!zsys_file_exists(config_file)) {
        fprintf(stderr, "missing config file: %s\n", config_file);
        exit(1);
    }

    setvbuf(stdout,NULL,_IOLBF,0);
    setvbuf(stderr,NULL,_IOLBF,0);

    // TODO: figure out sensible port numbers
    pub_port = pull_port + 1;

    printf("started logjam-device.\npull-port: %d\npub-port:  %d\nrabbit-host: %s\n",
           pull_port, pub_port, rabbit_host);

    // load config
    config = zconfig_load((char*)config_file);

    // create context
    zctx_t *context = zctx_new();
    assert_x(context==NULL, "zmq_init failed");

    // configure the context
    zctx_set_linger(context, 100);
    zctx_set_rcvhwm(context, 1000);
    // zctx_set_iothreads(context, 2);

    // create socket to receive messages on
    void *receiver = zsocket_new(context, ZMQ_PULL);
    assert_x(receiver==NULL, "zmq socket creation failed");

    //  configure the socket
    zsocket_set_rcvhwm(receiver, 100);
    zsocket_set_linger(receiver, 1000);

    rc = zsocket_bind(receiver, "tcp://%s:%d", "*", pull_port);
    assert_x(rc!=pull_port, "zmq socket bind failed");

    // create socket for publishing
    void *publisher = zsocket_new(context, ZMQ_PUB);
    assert_x(publisher==NULL, "zmq socket creation failed");

    zsocket_set_sndhwm(publisher, 1000);
    zsocket_set_linger(publisher, 1000);

    rc = zsocket_bind(publisher, "tcp://%s:%d", "*", pub_port);
    assert_x(rc!=pub_port, "zmq socket bind failed");

    /* test code
       zmsg_t *test_message = zmsg_new();
       zmsg_addstr(test_message, "testtesttest");
       zmsg_addstr(test_message, "data");
       zmsg_send(&test_message, publisher);
       assert(test_message==NULL);
    */

    // setup the rabbitmq listener thread
    void *rabbitmq_listener_pipe = zthread_fork(context, rabbitmq_listener, NULL);
    assert_x(rabbitmq_listener_pipe==NULL, "could not fork rabbitmq listener thread");
    printf("created rabbitmq listener thread\n");

    // configure amqp publisher connection
    amqp_connection_state_t connection = setup_amqp_connection();

    // so far, no exchanges are known
    exchanges = zhash_new();

    // set up event loop
    zloop_t *loop = zloop_new();
    assert(loop);
    zloop_set_verbose(loop, 0);

    // calculate statistics every 1000 ms
    int timer_id = 1;
    rc = zloop_timer(loop, 1000, 0, timer_event, &timer_id);
    assert(rc == 0);

    // setup handler for the receiver socket
    zmq_pollitem_t item;
    item.socket = receiver;
    item.events = ZMQ_POLLIN;
    publisher_state_t publisher_state = {publisher, connection, 0};
    rc = zloop_poller(loop, &item, read_zmq_message_and_forward, &publisher_state);
    assert(rc == 0);

    // setup handler for the rabbit listener pipe
    zmq_pollitem_t listener_item;
    publisher_state_t listener_state = {publisher, NULL, 0};
    listener_item.socket = rabbitmq_listener_pipe;
    listener_item.events = ZMQ_POLLIN;
    rc = zloop_poller(loop, &listener_item, read_zmq_message_and_forward, &listener_state);
    assert(rc == 0);

    rc = zloop_start(loop);
    // printf("zloop return: %d", rc);

    zloop_destroy(&loop);
    assert(loop == NULL);

    printf("received %lu messages", received_messages_count);

    // close zmq socket and context
    zsocket_destroy(context, receiver);
    zctx_destroy(&context);

    shutdown_amqp_connection(connection, publisher_state.amqp_rc);

    return (publisher_state.amqp_rc < 0);
}
