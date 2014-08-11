#include <assert.h>
#include <pthread.h>
#include <stdbool.h>

#include <directfb.h>
#include <zmq.h>
#include <zmq_utils.h>

#include <freeabode/freeabode.pb-c.h>
#include <freeabode/security.h>
#include <freeabode/util.h>

#define ADJUSTMENT_DELAY_SECS  1, 318
#define FONT_NAME  "DroidSansMono.ttf"

static IDirectFB *dfb;

static
void dfbassert(DFBResult err, const char *file, int line, const char *expr)
{
	if (err == DFB_OK)
		return;
	fprintf(stderr, "%s:%d: ", file, line);
	DirectFBErrorFatal(expr, err);
}
#define dfbassert(expr)  dfbassert(expr, __FILE__, __LINE__, #expr)

static void *my_zmq_context;

struct my_font {
	IDirectFBFont *dfbfont;
	int height;
	int ascender;
	int descender;
	int width_x;
};
struct my_font font_h2, font_h4;

static
void my_load_font(struct my_font * const myfont, const char * const fontname, const DFBFontDescription * const fontdsc)
{
	dfbassert(dfb->CreateFont(dfb, fontname, fontdsc, &myfont->dfbfont));
	dfbassert(myfont->dfbfont->GetHeight(myfont->dfbfont, &myfont->height));
	dfbassert(myfont->dfbfont->GetAscender(myfont->dfbfont, &myfont->ascender));
	dfbassert(myfont->dfbfont->GetDescender(myfont->dfbfont, &myfont->descender));
	dfbassert(myfont->dfbfont->GetStringWidth(myfont->dfbfont, "x", 1, &myfont->width_x));
}

static
void my_win_init(IDirectFBWindow * const window, IDirectFBSurface ** const out_surface, DFBDimension * const out_size)
{
	dfbassert(window->GetSurface(window, out_surface));
	if (out_size)
		dfbassert(window->GetSize(window, &out_size->w, &out_size->h));
	dfbassert((*out_surface)->Clear(*out_surface, 0, 0, 0, 0));
	dfbassert((*out_surface)->Flip(*out_surface, NULL, DSFLIP_BLIT));
	dfbassert(window->SetOpacity(window, 0xff));
}

static inline
int32_t decicelcius_to_millifahrenheit(int32_t dc)
{
	return dc * 90 / 5 + 32000;
}

struct weather_windows {
	IDirectFBWindow *temperature;
	IDirectFBWindow *humidity;
	IDirectFBWindow *hvac_indicator;
	IDirectFBWindow *charging_indicator;
};

static
void weather_thread(void * const userp)
{
	const struct weather_windows * const ww = userp;
	
	void *client_weather;
	client_weather = zmq_socket(my_zmq_context, ZMQ_SUB);
	freeabode_zmq_security(client_weather, false);
	assert(!zmq_connect(client_weather, "tcp://192.168.77.104:2929"));
	assert(!zmq_setsockopt(client_weather, ZMQ_SUBSCRIBE, NULL, 0));
	
	
	IDirectFBWindow * const window = ww->temperature;
	IDirectFBSurface *surface, *surface_hvac_indicator, *surface_charge, *surface_humidity;
	DFBDimension winsize_temp, winsize_hvac_indicator, winsize_charge;
	
	my_win_init(window, &surface, &winsize_temp);
	my_win_init(ww->hvac_indicator, &surface_hvac_indicator, &winsize_hvac_indicator);
	my_win_init(ww->charging_indicator, &surface_charge, &winsize_charge);
	my_win_init(ww->humidity, &surface_humidity, NULL);
	
	char buf[0x10];
	bool fetstatus[PB_HVACWIRES___COUNT] = {true,true,true,true,true,true,true,true,true,true,true,true};
	bool charging = false;
	int32_t fahrenheit = 0;
	unsigned humidity = 0;
	while (true)
	{
		PbEvent *pbevent;
		zmq_recv_protobuf(client_weather, pb_event, pbevent, NULL);
		
		PbWeather *weather = pbevent->weather;
		if (weather)
		{
			if (weather->has_temperature)
				fahrenheit = decicelcius_to_millifahrenheit(weather->temperature);
			if (weather->has_humidity)
				humidity = weather->humidity;
		}
		for (int i = 0; i < pbevent->n_wire_change; ++i)
			if (pbevent->wire_change[i]->wire < PB_HVACWIRES___COUNT && pbevent->wire_change[i]->wire > 0)
			fetstatus[pbevent->wire_change[i]->wire] = pbevent->wire_change[i]->connect;
		if (pbevent->battery && pbevent->battery->has_charging)
			charging = pbevent->battery->charging;
		
		{
			if (fetstatus[PB_HVACWIRES__Y1])
			{
				snprintf(buf, sizeof(buf), "%s%s", fetstatus[PB_HVACWIRES__OB] ? "Cool" : "Heat", fetstatus[PB_HVACWIRES__G] ? "" : " (no fan)");
			}
			else
			if (fetstatus[PB_HVACWIRES__G])
				strcpy(buf, "Fan");
			else
				strcpy(buf, "Off");
			
			dfbassert(surface_hvac_indicator->Clear(surface_hvac_indicator, 0xff, 0, 0, 0x1f));
			dfbassert(surface_hvac_indicator->SetColor(surface_hvac_indicator, 0x80, 0xff, 0x20, 0xff));
			dfbassert(surface_hvac_indicator->SetFont(surface_hvac_indicator, font_h4.dfbfont));
			dfbassert(surface_hvac_indicator->DrawString(surface_hvac_indicator, buf, -1, winsize_hvac_indicator.w / 2, font_h4.height, DSTF_CENTER));
			dfbassert(surface_hvac_indicator->Flip(surface_hvac_indicator, NULL, DSFLIP_BLIT));
		}
		
		{
			dfbassert(surface_charge->Clear(surface_charge, 0, 0, 0xff, 0x1f));
			if (charging)
			{
				dfbassert(surface_charge->SetColor(surface_charge, 0x80, 0xff, 0x20, 0xff));
				dfbassert(surface_charge->SetFont(surface_charge, font_h4.dfbfont));
				dfbassert(surface_charge->DrawString(surface_charge, "Charging", -1, winsize_charge.w / 2, font_h4.height, DSTF_CENTER));
			}
			dfbassert(surface_charge->Flip(surface_charge, NULL, DSFLIP_BLIT));
		}
		
		{
			snprintf(buf, sizeof(buf), "%02u", humidity / 10);
			dfbassert(surface->Clear(surface_humidity, 0xff, 0, 0, 0x1f));
			dfbassert(surface->SetColor(surface_humidity, 0x80, 0xff, 0x20, 0xff));
			dfbassert(surface->SetFont(surface_humidity, font_h2.dfbfont));
			dfbassert(surface->DrawString(surface_humidity, buf, -1, 0, font_h2.height, DSTF_LEFT));
			dfbassert(surface->Flip(surface_humidity, NULL, DSFLIP_BLIT));
		}
		
		snprintf(buf, sizeof(buf), "%2u", (unsigned)(fahrenheit / 1000));
		
		dfbassert(surface->Clear(surface, 0, 0xff, 0, 0x1f));
		dfbassert(surface->SetColor(surface, 0x80, 0xff, 0x20, 0xff));
		dfbassert(surface->SetFont(surface, font_h2.dfbfont));
		dfbassert(surface->DrawString(surface, buf, -1, winsize_temp.w, font_h2.height, DSTF_RIGHT));
		dfbassert(surface->Flip(surface, NULL, DSFLIP_BLIT));
	}
}

static int current_goal;
static bool adjusting;
static int adjusted_goal;
static int adjusting_pipe[2];
static void *client_tstat_ctl;

static
void init_client_tstat_ctl()
{
	client_tstat_ctl = zmq_socket(my_zmq_context, ZMQ_REQ);
	freeabode_zmq_security(client_tstat_ctl, false);
	assert(!zmq_connect(client_tstat_ctl, "tcp://192.168.77.104:2932"));
}

static
void goal_thread(void * const userp)
{
	void *client_tstat;
	client_tstat = zmq_socket(my_zmq_context, ZMQ_SUB);
	freeabode_zmq_security(client_tstat, false);
	assert(!zmq_connect(client_tstat, "tcp://192.168.77.104:2931"));
	assert(!zmq_setsockopt(client_tstat, ZMQ_SUBSCRIBE, NULL, 0));
	
	IDirectFBWindow * const window = userp;
	IDirectFBSurface *surface;
	DFBDimension winsize_tempgoal;
	my_win_init(window, &surface, &winsize_tempgoal);
	
	zmq_pollitem_t pollitems[] = {
		{ .socket = client_tstat, .events = ZMQ_POLLIN },
		{ .fd = adjusting_pipe[0], .events = ZMQ_POLLIN },
	};
	
	char buf[0x10];
	while (true)
	{
		if (zmq_poll(pollitems, sizeof(pollitems) / sizeof(*pollitems), -1) <= 0)
			continue;
		
		if (pollitems[0].revents & ZMQ_POLLIN)
		{
			PbEvent *pbevent;
			zmq_recv_protobuf(client_tstat, pb_event, pbevent, NULL);
			
			PbHVACGoals *goals = pbevent->hvacgoals;
			if (goals && goals->has_temp_high)
			{
				current_goal = goals->temp_high;
				if (!client_tstat_ctl)
					init_client_tstat_ctl();
			}
		}
		if (pollitems[1].revents & ZMQ_POLLIN)
			read(adjusting_pipe[0], buf, sizeof(buf));
		
		int fahrenheit_current  = decicelcius_to_millifahrenheit(current_goal ) / 1000;
		int fahrenheit_adjusted;
		if (adjusting)
			fahrenheit_adjusted = decicelcius_to_millifahrenheit(adjusted_goal) / 1000;
		else
			fahrenheit_adjusted = fahrenheit_current;
		
		snprintf(buf, sizeof(buf), "%2u", (unsigned)(fahrenheit_adjusted));
		
		dfbassert(surface->Clear(surface, 0, 0, 0xff, 0x1f));
		if (fahrenheit_adjusted == fahrenheit_current)
			dfbassert(surface->SetColor(surface, 0x80, 0xff, 0x20, 0xff));
		else
			dfbassert(surface->SetColor(surface, 0xff, 0x80, 0x20, 0xff));
		dfbassert(surface->SetFont(surface, font_h4.dfbfont));
		dfbassert(surface->DrawString(surface, buf, -1, winsize_tempgoal.w, font_h4.height, DSTF_RIGHT));
		dfbassert(surface->Flip(surface, NULL, DSFLIP_BLIT));
	}
}

// right is negative, left is positive
static
void handle_knob_turn(const int axisrel)
{
	if (!client_tstat_ctl)
		return;
	if (!adjusting)
	{
		adjusting = true;
		adjusted_goal = current_goal;
	}
	adjusted_goal -= axisrel;
	write(adjusting_pipe[1], "", 1);
}

static
void make_adjustments()
{
	{
		PbRequest req = PB_REQUEST__INIT;
		PbHVACGoals goals = PB_HVACGOALS__INIT;
		goals.has_temp_high = true;
		goals.temp_high = adjusted_goal;
		req.hvacgoals = &goals;
		zmq_send_protobuf(client_tstat_ctl, pb_request, &req, 0);
	}
	
	{
		PbRequestReply *reply;
		zmq_recv_protobuf(client_tstat_ctl, pb_request_reply, reply, NULL);
		bool success = false;
		if (reply->hvacgoals && reply->hvacgoals->has_temp_high)
		{
			current_goal = reply->hvacgoals->temp_high;
			success = (reply->hvacgoals->temp_high == adjusted_goal);
				success = true;
		}
		pb_request_reply__free_unpacked(reply, NULL);
		if (!success)
			return;
	}
	
	adjusting = false;
	write(adjusting_pipe[1], "", 1);
}

static
void handle_button_press()
{
	// TODO
}

int main(int argc, char **argv)
{
	load_freeabode_key();
	my_zmq_context = zmq_ctx_new();
	assert(!pipe(adjusting_pipe));
	
	IDirectFBDisplayLayer *layer;
	struct weather_windows weather_windows;
	
	dfbassert(DirectFBInit(&argc, &argv));
	dfbassert(DirectFBCreate(&dfb));
	dfbassert(dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer));
	dfbassert(layer->SetCooperativeLevel(layer, DLSCL_ADMINISTRATIVE));
	dfbassert(layer->EnableCursor(layer, 0));
	dfbassert(layer->SetBackgroundMode(layer, DLBM_COLOR));
	dfbassert(layer->SetBackgroundColor(layer, 0, 0, 0, 0xff));
	IDirectFBSurface *surface;
	dfbassert(layer->GetSurface(layer, &surface));
	{
		int width, height;
		dfbassert(surface->GetSize(surface, &width, &height));
		const int center_x = width / 2;
		const int center_y = height / 2;
		
		DFBFontDescription font_dsc = {
			.flags = DFDESC_WIDTH,
			.width = width * 2 / 3 / 4,
		};
		my_load_font(&font_h2, FONT_NAME, &font_dsc);
		
		font_dsc.width /= 2;
		my_load_font(&font_h4, FONT_NAME, &font_dsc);
		
		IDirectFBWindow *window;
		DFBWindowDescription windesc = {
			.flags = DWDESC_CAPS | DWDESC_WIDTH | DWDESC_HEIGHT | DWDESC_POSX | DWDESC_POSY | DWDESC_OPTIONS,
			.caps = DWCAPS_ALPHACHANNEL,
			.width = (width * 2 / 3 / 2) - (font_h2.width_x / 2),
			.height = font_h2.height - font_h2.descender,
			.posy = 0,
			.options = DWOP_ALPHACHANNEL,
		};
		windesc.posx = center_x - (windesc.width + font_h2.width_x / 2);
		windesc.posy = (height / 2) - (font_h2.height - font_h2.ascender);
		dfbassert(layer->CreateWindow(layer, &windesc, &window));
		weather_windows = (struct weather_windows){
			.temperature = window,
		};
		
		windesc.posx += windesc.width + font_h2.width_x;
		dfbassert(layer->CreateWindow(layer, &windesc, &window));
		weather_windows.humidity = window;
		windesc.posx -= windesc.width + font_h2.width_x;
		
		windesc.height = font_h4.height - font_h4.descender;
		windesc.posy += font_h2.height;
		dfbassert(layer->CreateWindow(layer, &windesc, &window));
		zmq_threadstart(goal_thread, window);
		
		windesc.width = width * 2 / 3;
		windesc.posx = center_x - (windesc.width / 2);
		windesc.posy += font_h4.height;
		dfbassert(layer->CreateWindow(layer, &windesc, &window));
		weather_windows.hvac_indicator = window;
		
		windesc.posy += font_h4.height;
		dfbassert(layer->CreateWindow(layer, &windesc, &window));
		weather_windows.charging_indicator = window;
		
		zmq_threadstart(weather_thread, &weather_windows);
	}
	
	// Main thread now handles input
	IDirectFBEventBuffer *evbuf;
	DFBEvent ev;
	dfbassert(dfb->CreateInputEventBuffer(dfb, DICAPS_ALL, DFB_TRUE, &evbuf));
	while (true)
	{
		{
			DFBResult res;
			if (adjusting)
			{
				res = evbuf->WaitForEventWithTimeout(evbuf, ADJUSTMENT_DELAY_SECS);
				if (res == DFB_TIMEOUT)
				{
					// Timeout occurred
					make_adjustments();
					continue;
				}
			}
			else
				res = evbuf->WaitForEvent(evbuf);
			if (res == DFB_INTERRUPTED)
				// Shouldn't happen, but does :(
				continue;
			dfbassert(res);
		}
		dfbassert(evbuf->GetEvent(evbuf, &ev));
		if (ev.clazz != DFEC_INPUT)
			continue;
		
		switch (ev.input.type)
		{
			case DIET_AXISMOTION:
				// Knob
				if (!(ev.input.flags & DIEF_AXISREL))
					break;
				
				handle_knob_turn(ev.input.axisrel);
				
				break;
			case DIET_KEYPRESS:
			case DIET_KEYRELEASE:
				if (ev.input.flags & DIEF_KEYID)
				{
					// Simulate knob turn for left/right arrow keys
					if (ev.input.key_id == DIKI_LEFT)
					{
						handle_knob_turn(4);
						break;
					}
					else
					if (ev.input.key_id == DIKI_RIGHT)
					{
						handle_knob_turn(-4);
						break;
					}
				}
				
				handle_button_press();
				
				break;
			default:
				break;
		}
	}
}
