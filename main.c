#include "config.h"

#include <assert.h>
#include <stdio.h>

#include <zmq.h>

#include "nest.h"
#include "freeabode.pb-c.h"
#include "security.h"
#include "util.h"

static const int periodic_req_interval = 30;

static void *my_zmq_context, *my_zmq_publisher;
static struct timespec ts_last_periodic_req;

static
void request_periodic(struct nbp_device *nbp, const struct timespec *now)
{
	ts_last_periodic_req = *now;
	nbp_send(nbp, NBPM_REQ_PERIODIC, NULL, 0);
#ifdef DEBUG_NBP
	puts("Periodic data request");
#endif
}

#ifdef DEBUG_NBP
void debug_msg(struct nbp_device * const nbp, const struct timespec * const now, const enum nbp_message_type mtype, const void * const data, const size_t datasz)
{
	char hexdata[(datasz * 2) + 1];
	bin2hex(hexdata, data, datasz);
	printf("msg %04x data %s\n", mtype, hexdata);
}
#endif

static
void reset_complete(struct nbp_device *nbp, const struct timespec *now, uint16_t fet_bitmask)
{
	nbp->cb_msg_fet_presence = NULL;
	printf("Backplate reset complete\n");
	
	request_periodic(nbp, now);
	
	my_zmq_context = zmq_ctx_new();
	
	start_zap_handler(my_zmq_context);
	
	my_zmq_publisher = zmq_socket(my_zmq_context, ZMQ_PUB);
	
	freeabode_zmq_security(my_zmq_publisher, true);
	assert(!zmq_bind(my_zmq_publisher, "tcp://*:2929"));
	assert(!zmq_bind(my_zmq_publisher, "ipc://weather.ipc"));
}

void msg_log(struct nbp_device *nbp, const struct timespec *now, const char *msg)
{
	printf("Backplate: %s\n", msg);
}

void msg_weather(struct nbp_device *nbp, const struct timespec *now, uint16_t temperature, uint16_t humidity)
{
	int32_t fahrenheit = ((int32_t)temperature) * 90 / 5 + 32000;
	printf("Temperature %3d.%02d C (%4d.%03d F)    Humidity: %d.%d%%\n", temperature / 100, temperature % 100, fahrenheit / 1000, fahrenheit % 1000, humidity / 10, humidity % 10);
	
	PbWeather pb = PB_WEATHER__INIT;
	pb.has_temperature = true;
	pb.temperature = temperature;
	pb.has_humidity = true;
	pb.humidity = humidity;
	zmq_send_protobuf(my_zmq_publisher, pb_weather, &pb, 0);
}

int main(int argc, char **argv)
{
	load_freeabode_key();
	
	struct nbp_device *nbp = nbp_open("/dev/ttyO2");
	assert(nbp_send(nbp, NBPM_RESET, NULL, 0));
#ifdef DEBUG_NBP
	nbp->cb_msg = debug_msg;
#endif
	nbp->cb_msg_fet_presence = reset_complete;
	nbp->cb_msg_log = msg_log;
	nbp->cb_msg_weather = msg_weather;
	
	struct timespec ts_now;
	zmq_pollitem_t pollitems[] = {
		{ .fd = nbp->_fd, .events = ZMQ_POLLIN },
	};
	while (true)
	{
		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		if (ts_now.tv_sec - periodic_req_interval > ts_last_periodic_req.tv_sec)
			request_periodic(nbp, &ts_now);
		if (zmq_poll(pollitems, sizeof(pollitems) / sizeof(*pollitems), -1) <= 0)
			continue;
		if (pollitems[0].revents & ZMQ_POLLIN)
			nbp_read(nbp);
	}
}
