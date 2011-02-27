// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

// Server brick (just for demonstration)

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING
//#define IO_DEBUGGING

#ifdef IO_DEBUGGING
#define MARS_IO MARS_DBG
#else
#define MARS_IO(args...) /*empty*/
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/kthread.h>

#define _STRATEGY
#include "mars.h"

///////////////////////// own type definitions ////////////////////////

#include "mars_server.h"

static struct socket *server_socket = NULL;
static struct task_struct *server_thread = NULL;

///////////////////////// own helper functions ////////////////////////

static int server_checker(const char *path, const char *name, int namlen, unsigned int d_type, int *prefix, int *serial)
{
	return 0;
}

static int server_worker(struct mars_global *global, struct mars_dent *dent, bool direction)
{
	return 0;
}

static void server_endio(struct generic_callback *cb)
{
	struct server_mref_aspect *mref_a;
	struct mref_object *mref;
	struct server_brick *brick;
	struct socket **sock;
	int status;

	mref_a = cb->cb_private;
	CHECK_PTR(mref_a, err);
	mref = mref_a->object;
	CHECK_PTR(mref, err);
	brick = mref_a->brick;
	CHECK_PTR(brick, err);
	sock = mref_a->sock;
	CHECK_PTR(sock, err);

	down(&brick->socket_sem);
	status = mars_send_cb(sock, mref);
	up(&brick->socket_sem);

	if (status < 0) {
		MARS_ERR("cannot send response, status = %d\n", status);
		kernel_sock_shutdown(*sock, SHUT_WR);
	}
	atomic_dec(&brick->in_flight);
	return;
err:
	MARS_FAT("cannot handle callback - giving up\n");
}

static int handler_thread(void *data)
{
	struct server_brick *brick = data;
	struct socket **sock = &brick->handler_socket;
	int max_round = 300;
	int timeout;
	int status = 0;

	brick->handler_thread = NULL;
	wake_up_interruptible(&brick->startup_event);
	MARS_DBG("--------------- handler_thread starting on socket %p\n", *sock);
	if (!*sock)
		goto done;

	//fake_mm();

        while (!kthread_should_stop()) {
		struct mars_cmd cmd = {};

		status = mars_recv_struct(sock, &cmd, mars_cmd_meta);
		if (status < 0) {
			MARS_ERR("command status = %d\n", status);
			break;
		}

		MARS_IO("cmd = %d\n", cmd.cmd_code);

		status = -EPROTO;
		switch (cmd.cmd_code) {
		case CMD_NOP:
			MARS_DBG("got NOP operation\n");
			status = 0;
			break;
		case CMD_STATUS:
			//...
			MARS_ERR("NYI\n");
			break;
		case CMD_GETINFO:
		{
			struct mars_info info = {};
			status = GENERIC_INPUT_CALL(brick->inputs[0], mars_get_info, &info);
			if (status < 0) {
				break;
			}
			status = mars_send_struct(sock, &cmd, mars_cmd_meta);
			if (status < 0) {
				break;
			}
			status = mars_send_struct(sock, &info, mars_info_meta);
			break;
		}
		case CMD_GETENTS:
		{
			struct mars_global glob_tmp = {
				.dent_anchor = LIST_HEAD_INIT(glob_tmp.dent_anchor),
				.brick_anchor = LIST_HEAD_INIT(glob_tmp.brick_anchor),
				.mutex = __SEMAPHORE_INITIALIZER(glob_tmp.mutex, 1),
			};

			status = -EINVAL;
			if (!cmd.cmd_str1)
				break;

			status = mars_dent_work(&glob_tmp, cmd.cmd_str1, sizeof(struct mars_dent), server_checker, server_worker, NULL, cmd.cmd_int1);
			MARS_DBG("dents status = %d\n", status);
			if (status < 0)
				break;

			down(&brick->socket_sem);
			status = mars_send_dent_list(sock, &glob_tmp.dent_anchor);
			up(&brick->socket_sem);

			if (status < 0) {
				MARS_ERR("could not send dentry information, status = %d\n", status);
			}

			mars_dent_free_all(&glob_tmp.dent_anchor);
			break;
		}
		case CMD_CONNECT:
		{
			struct mars_brick *prev;

			//TODO: fix possible races
			prev = mars_find_brick(mars_global, NULL, cmd.cmd_str1);			if (likely(prev)) {
				status = generic_connect((void*)brick->inputs[0], (void*)prev->outputs[0]);
			} else {
				MARS_ERR("cannot find brick '%s'\n", cmd.cmd_str1 ? cmd.cmd_str1 : "NULL");
				status = -EINVAL;
			}

			cmd.cmd_int1 = status;
			status = mars_send_struct(sock, &cmd, mars_cmd_meta);
			break;
		}
		case CMD_MREF:
		{
			struct mref_object *mref;
			struct server_mref_aspect *mref_a;

			mref = server_alloc_mref(&brick->hidden_output, &brick->mref_object_layout);
			status = -ENOMEM;
			if (!mref)
				break;
			mref_a = server_mref_get_aspect(&brick->hidden_output, mref);
			if (unlikely(!mref_a)) {
				kfree(mref);
				break;
			}

			status = mars_recv_mref(sock, mref);
			if (status < 0)
				break;

			mref_a->brick = brick;
			mref_a->sock = sock;
			mref->_ref_cb.cb_private = mref_a;
			mref->_ref_cb.cb_fn = server_endio;
			mref->ref_cb = &mref->_ref_cb;
			atomic_inc(&brick->in_flight);

			status = GENERIC_INPUT_CALL(brick->inputs[0], mref_get, mref);
			if (status < 0) {
				MARS_INF("execution error = %d\n", status);
				mref->_ref_cb.cb_error = status;
				server_endio(&mref->_ref_cb);
				mars_free_mref(mref);
				status = 0; // continue serving requests
				break;
			}

			GENERIC_INPUT_CALL(brick->inputs[0], mref_io, mref);
			GENERIC_INPUT_CALL(brick->inputs[0], mref_put, mref);
			break;
		}
		case CMD_CB:
			MARS_ERR("oops, as a server I should never get CMD_CB; something is wrong here - attack attempt??\n");
			break;
		default:
			MARS_ERR("unknown command %d\n", cmd.cmd_code);
		}
		if (status < 0)
			break;
	}

	//kernel_sock_shutdown(*sock, SHUT_WR);
	sock_release(*sock);
	//cleanup_mm();

done:
	MARS_DBG("handler_thread terminating, status = %d\n", status);
	mars_power_button((void*)brick, false, true);
	do {
		int status;
		status = brick->ops->brick_switch(brick);
		if (status < 0) {
			MARS_ERR("server shutdown failed, status = %d\n", status);
		} else if (max_round-- < 0)
			break;
		msleep(1000);
	} while (!brick->power.led_off);

	if (brick->inputs[0] && brick->inputs[0]->connect) {
		MARS_DBG("disconnecting input %p\n", brick->inputs[0]->connect);
		(void)generic_disconnect((void*)brick->inputs[0]);
	}

	timeout = 60 * 1000;
	while (atomic_read(&brick->in_flight) || !brick->power.led_off) {
		MARS_ERR("server brick has resources allocated - cannot terminate thread\n");
		msleep(timeout);
		if (timeout < 3600 * 1000)
			timeout += 30 * 1000;
	}

	(void)generic_brick_exit_full((void*)brick);
	MARS_DBG("done\n");
	return 0;
}

////////////////// own brick / input / output operations //////////////////

static int server_get_info(struct server_output *output, struct mars_info *info)
{
	struct server_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, mars_get_info, info);
}

static int server_ref_get(struct server_output *output, struct mref_object *mref)
{
	struct server_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, mref_get, mref);
}

static void server_ref_put(struct server_output *output, struct mref_object *mref)
{
	struct server_input *input = output->brick->inputs[0];
	GENERIC_INPUT_CALL(input, mref_put, mref);
}

static void server_ref_io(struct server_output *output, struct mref_object *mref)
{
	struct server_input *input = output->brick->inputs[0];
	GENERIC_INPUT_CALL(input, mref_io, mref);
}

static int server_switch(struct server_brick *brick)
{
	if (brick->power.button) {
		mars_power_led_off((void*)brick, false);

		MARS_INF("starting.....");
		
		mars_power_led_on((void*)brick, true);
	} else {
		mars_power_led_on((void*)brick, false);
		mars_power_led_off((void*)brick, true);
	}
	return 0;
}

//////////////// object / aspect constructors / destructors ///////////////

static int server_mref_aspect_init_fn(struct generic_aspect *_ini, void *_init_data)
{
	struct server_mref_aspect *ini = (void*)_ini;
	(void)ini;
	return 0;
}

static void server_mref_aspect_exit_fn(struct generic_aspect *_ini, void *_init_data)
{
	struct server_mref_aspect *ini = (void*)_ini;
	(void)ini;
}

MARS_MAKE_STATICS(server);

////////////////////// brick constructors / destructors ////////////////////

static int server_brick_construct(struct server_brick *brick)
{
	struct server_output *hidden = &brick->hidden_output;
	_server_output_init(brick, hidden, "internal");
	init_waitqueue_head(&brick->startup_event);
	sema_init(&brick->socket_sem, 1);
	return 0;
}

static int server_output_construct(struct server_output *output)
{
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct server_brick_ops server_brick_ops = {
	.brick_switch = server_switch,
};

static struct server_output_ops server_output_ops = {
	.make_object_layout = server_make_object_layout,
	.mars_get_info = server_get_info,
	.mref_get = server_ref_get,
	.mref_put = server_ref_put,
	.mref_io = server_ref_io,
};

const struct server_input_type server_input_type = {
	.type_name = "server_input",
	.input_size = sizeof(struct server_input),
};

static const struct server_input_type *server_input_types[] = {
	&server_input_type,
};

const struct server_output_type server_output_type = {
	.type_name = "server_output",
	.output_size = sizeof(struct server_output),
	.master_ops = &server_output_ops,
	.output_construct = &server_output_construct,
	.aspect_types = server_aspect_types,
	.layout_code = {
		[BRICK_OBJ_MREF] = LAYOUT_ALL,
	}
};

static const struct server_output_type *server_output_types[] = {
	&server_output_type,
};

const struct server_brick_type server_brick_type = {
	.type_name = "server_brick",
	.brick_size = sizeof(struct server_brick),
	.max_inputs = 1,
	.max_outputs = 0,
	.master_ops = &server_brick_ops,
	.default_input_types = server_input_types,
	.default_output_types = server_output_types,
	.brick_construct = &server_brick_construct,
};
EXPORT_SYMBOL_GPL(server_brick_type);

///////////////////////////////////////////////////////////////////////

// strategy layer

static int _server_thread(void *data)
{
	char *id = my_id();
	int version = 0;
	int status = 0;

	//fake_mm();

	MARS_INF("-------- server starting on host '%s' ----------\n", id);

        while (!kthread_should_stop()) {
		int size;
		struct server_brick *brick;
		struct task_struct *thread;
		struct socket *new_socket = NULL;
		int status;
		status = kernel_accept(server_socket, &new_socket, O_NONBLOCK);
		if (status < 0) {
			msleep(500);
			if (status == -EAGAIN)
				continue; // without error message
			MARS_ERR("accept status = %d\n", status);
			continue;
		}
		if (!new_socket) {
			MARS_ERR("got no socket\n");
			msleep(3000);
			continue;
		}
		MARS_DBG("got new connection %p\n", new_socket);

		/* TODO: check authorization.
		 */

		size = server_brick_type.brick_size +
			(server_brick_type.max_inputs + server_brick_type.max_outputs) * sizeof(void*) +
			sizeof(struct server_input),

		brick = kzalloc(size, GFP_MARS);
		if (!brick) {
			MARS_ERR("cannot allocate server instance\n");
			goto err;
		}
		
		status = generic_brick_init_full(brick, size, (void*)&server_brick_type, NULL, NULL, NULL);
		if (status) {
			MARS_ERR("cannot init server brick, status = %d\n", status);
			goto err;
		}

		thread = kthread_create(handler_thread, brick, "mars_handler%d", version++);
		if (IS_ERR(thread)) {
			MARS_ERR("cannot create thread, status = %ld\n", PTR_ERR(thread));
			goto err;
		}
		brick->handler_thread = thread;
		brick->handler_socket = new_socket;
		wake_up_process(thread);
		wait_event_interruptible(brick->startup_event, brick->handler_thread == NULL);
		continue;

	err:
		if (new_socket) {
			kernel_sock_shutdown(new_socket, SHUT_WR);
			sock_release(new_socket);
		}
	}

	MARS_INF("-------- cleaning up ----------\n");

	//cleanup_mm();

	MARS_INF("-------- done status = %d ----------\n", status);
	server_thread = NULL;
	return status;
}

////////////////// module init stuff /////////////////////////

static int __init init_server(void)
{
	struct sockaddr_storage sockaddr = {};
	struct task_struct *thread;
	int status;

	MARS_INF("init_server()\n");
	
	status = mars_create_sockaddr(&sockaddr, "");
	if (status < 0)
		return status;

	status = mars_create_socket(&server_socket, &sockaddr, true);
	if (status < 0)
		return status;

	status = kernel_listen(server_socket, 100);
	if (status < 0)
		return status;

	thread = kthread_create(_server_thread, NULL, "mars_server");
	if (IS_ERR(thread)) {
		return PTR_ERR(thread);
	}

	server_thread = thread;
	wake_up_process(thread);

	return server_register_brick_type();
}

static void __exit exit_server(void)
{
	MARS_INF("exit_server()\n");
	server_unregister_brick_type();
	if (server_thread) {
		if (server_socket) {
			kernel_sock_shutdown(server_socket, SHUT_WR);
		}
		kthread_stop(server_thread);
		if (server_socket && !server_thread) {
			sock_release(server_socket);
			server_socket = NULL;
		}
	}
}

MODULE_DESCRIPTION("MARS server brick");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_server);
module_exit(exit_server);
