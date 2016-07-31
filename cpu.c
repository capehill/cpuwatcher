/*

CPU Watcher 0.6 by Juha Niemimäki (c) 2005 - 2016

- measures CPU, free memory and network traffic.


Special thanks to Thomas Frieden, Olaf Barthel, Alex Carmona and Dave Fisher.

TODO:
- prefs editor

Change log:

0.6
- requires graphics.library V54
- replaced deprecated system calls
- fixed some task sync issues
- removed pseudo transparency
- code cleanup and refactoring
- started a GitHub project

0.5
- added tooltypes support
- added a work around for extra blinkering when Intuition is set to "solid window dragging" mode (reported by AlexC)

0.4
- GCC 4.0.3
- DSI fix
- new icon

0.3
- network graphs
- pseudo transparency
- optional non-busy looping method for CPU measuring
- dragbarless mode

0.2
- grid
- keyboard commands

0.1
- initial version with CPU & memory graphs

*/

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/dos.h>

#include <dos/dos.h>
#include <workbench/startup.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __attribute__((used)) char *version_string = "$VER: CPU Watcher 0.6 (31.7.2016)";

#define WINDOW_TITLE_FORMAT "CPU: %3d%% V: %3d%% P: %3d%% G: %3d%%"
#define SCREEN_TITLE_FORMAT "CPU: %3d%% Virtual: %3d%% Public: %3d%% Graphics: %3d%% Download: %4.1fKB/s Upload: %4.1fKB/s Mode: %c"

#define WINDOW_TITLE_LEN 64
#define SCREEN_TITLE_LEN 128

#define MINUTES 5
#define XSIZE (60 * MINUTES)

// 0...100 %
#define YSIZE 101

// Graph colors
#define CPU_COL 	0x0000A000 // Green
#define PUB_COL 	0x00FF1010 // Red
#define VIRT_COL	0x001010FF // Blue
#define VID_COL 	0x0010C0F0 // Brighter blue
#define GRID_COL 	0x00003000 // Dark green
#define DL_COL      0x0000A000 // Green
#define UL_COL      0x00FF1010 // Red
#define BG_COL		0x00000000

extern struct Library *GfxBase;
struct TimerIFace *ITimer = NULL;

typedef struct {
	BOOL cpu;
	BOOL grid;
	BOOL public_mem;
	BOOL virtual_mem;
	BOOL video_mem;
	BOOL solid_draw;
	BOOL net;
	BOOL transparent;
	BOOL dragbar;
} Features;

typedef struct {
	ULONG cpu;
	ULONG public_mem;
	ULONG virtual_mem;
	ULONG video_mem;
	ULONG grid;
	ULONG background;
	ULONG upload;
	ULONG download;
} Colors;

typedef struct {
	UBYTE cpu;
	UBYTE virtual_mem;
	UBYTE public_mem;
	UBYTE video_mem;
	UBYTE upload;
	UBYTE download;
} Sample;

typedef struct {
	struct TimeVal start;
	struct TimeVal finish;
	struct TimeVal total;
} IdleTime;

static IdleTime idle_time;

typedef struct {
	struct Window *window;
	struct BitMap *bm;
	struct MsgPort *timer_port;
	struct MsgPort *user_port;

	struct Task *main_task;
	struct Task *idle_task;

	BYTE idle_sig;
	BYTE main_sig;

	struct TimeRequest *timer_req;
		
	BYTE timer_device;
	struct TimeVal tv;
		
	APTR base_address;
	uint32 bytes_per_row;
	uint32 lpr;
		
	// This set of flags controls which graphs are drawn
	//ULONG show_bits;

	int x_pos;
	int y_pos;

     // Corresponds to seconds ran
	ULONG iter;

	volatile BOOL running;

	volatile BOOL idler_trouble;

    // Simple mode switches to non-busy looping option when measuring the CPU usage.
	volatile BOOL simple_mode;

    // How many times idle task was ran during 1 second. Run count 0 means 100% cpu usage, 100 means 0 % CPU usage
	volatile ULONG run_count;

	STRPTR window_title;
	STRPTR screen_title;

	Features features;

	Colors colors;

	Sample *samples;

	float dl_speed;
	float ul_speed;

} Context;

#define get_ptr(name) &ctx->samples[0].name
#define get_cur(name) ctx->samples[ctx->iter].name

// network.c
void init_netstats(void);
BOOL update_netstats(UBYTE *, UBYTE *, float *, float *, float *, float *);

// Idle task gives up CPU
static void my_switch(void)
{
	GetSysTime(&idle_time.finish);

	SubTime(&idle_time.finish, &idle_time.start);

	AddTime(&idle_time.total, &idle_time.finish);
}

// Idle task gets CPU
static void my_launch(void)
{
	GetSysTime(&idle_time.start);
}

static void idle_sleep(Context *ctx, struct TimeRequest *pause_req)
{
	struct TimeVal dest, source;

	ctx->run_count++;

	GetSysTime(&dest);

	source.Seconds = 0;
	source.Microseconds = 10000;

	AddTime(&dest, &source);

	pause_req->Request.io_Command = TR_ADDREQUEST;
	pause_req->Time.Seconds = dest.Seconds;
	pause_req->Time.Microseconds = dest.Microseconds;

	DoIO((struct IORequest *) pause_req);
}

static void idler(uint32 p1)
{
    // Used by idle task for 1/100 second pauses when running in non-busy looping mode
	struct TimeRequest *pause_req = NULL;
	struct MsgPort *idle_port = NULL;
	Context *ctx = (Context *)p1;

	idle_port = AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Name, "idler_port",
		TAG_DONE);
		
	if (!idle_port) {
		ctx->idler_trouble = TRUE;
		goto die;
	}

	pause_req = AllocSysObjectTags(ASOT_IOREQUEST,
		ASOIOR_Size, sizeof(struct TimeRequest),
		ASOIOR_ReplyPort, idle_port,
		ASOIOR_Duplicate, ctx->timer_req,
		TAG_DONE);

	if (!pause_req) {
		ctx->idler_trouble = TRUE;
		goto die;
	}

	ctx->idle_sig = AllocSignal(-1);

	if (ctx->idle_sig == -1) {
		ctx->idler_trouble = TRUE;
		goto die;
	}

	// Signal main task that we are ready
	Signal(ctx->main_task, 1L << ctx->main_sig);

	// Wait for main task
	Wait(1L << ctx->idle_sig);

	Forbid();
	ctx->idle_task->tc_Switch = my_switch;
	ctx->idle_task->tc_Launch = my_launch;
	ctx->idle_task->tc_Flags |= TF_SWITCH | TF_LAUNCH;
	Permit();

	// Use minimum priority
	SetTaskPri(ctx->idle_task, -127);

	while (ctx->running) {
		if (ctx->simple_mode) {
			idle_sleep(ctx, pause_req);
		}
	}

	Forbid();
	ctx->idle_task->tc_Switch = NULL;
	ctx->idle_task->tc_Launch = NULL;
	ctx->idle_task->tc_Flags ^= TF_SWITCH | TF_LAUNCH;
	Permit();

die:
	if (ctx->idle_sig != -1) {
		FreeSignal( ctx->idle_sig );
		ctx->idle_sig = -1;
	}

	if (pause_req) {
		FreeSysObject(ASOT_IOREQUEST, pause_req);
	}

	if (idle_port) {
		FreeSysObject(ASOT_PORT, idle_port);
	}

	// Tell the main task that we can leave now (error flag may be set!)
	Signal(ctx->main_task, 1L << ctx->main_sig);

	// Waiting for termination
	Wait(0L);
}

static void plot_vertical(Context *ctx, int x, int start, int end, ULONG color)
{
	int y;
	ULONG *ptr = (ULONG *)ctx->base_address + x + (YSIZE - 1 - end) * ctx->lpr;

	for (y = start; y <= end; y++) {
		*ptr = color;
		ptr += ctx->lpr;
	}
}


static void plot(Context *ctx, const UBYTE* const array, const ULONG color)
{
	int x;
	ULONG *ptr = ctx->base_address;

	for (x = 0; x < XSIZE; x++) {
		int iter = (ctx->iter + 1 + x) % XSIZE;
		
		UBYTE cur_y = *(array + iter * sizeof(Sample));

		// Plot the current level
		*(ptr + x + (YSIZE - 1 - cur_y) * ctx->lpr) = color;

		if (x > 0 && ctx->features.solid_draw) {

			int prev = (ctx->iter + x) % XSIZE;

			UBYTE prev_y = *(array + prev * sizeof(Sample));
			
			int diff = cur_y - prev_y;

			if (diff < 0) {
				plot_vertical(ctx, x, cur_y, prev_y, color);
			} else if (diff > 0) {
				plot_vertical(ctx, x, prev_y, cur_y, color);
			}
		}
	}
}

/* Special function to plot 2-sided network graph */
static void plot_net(Context *ctx)
{
	int x, y;
	ULONG *ptr = (ULONG *)ctx->base_address + ctx->lpr * YSIZE;

	UBYTE *ul = get_ptr(upload);
	UBYTE *dl = get_ptr(download);

	for (x = 0; x < XSIZE; x++) {
		int iter = (ctx->iter + 1 + x) % XSIZE;

		// Upload, above the center line
		int start_y = - *(ul + iter * sizeof(Sample)) / 2;
		ULONG *_ptr = ptr + x + (YSIZE / 2 + start_y) * ctx->lpr;

		for (y = start_y; y < 0; y++) {
			*_ptr = ctx->colors.upload;
			_ptr += ctx->lpr;
		}

		// Download, below the center line
		int end_y = *(dl + iter * sizeof(Sample)) / 2;
		_ptr = ptr + x + (YSIZE / 2 + 1) * ctx->lpr;

		for (y = 1 ; y <= end_y; y++) {
			*_ptr = ctx->colors.download;
			_ptr += ctx->lpr;
		}
	}
}

static void clear(Context *ctx)
{
	int x, y;
	ULONG *ptr = ctx->base_address;

	WORD height = ctx->window->Height - (ctx->window->BorderBottom + ctx->window->BorderTop);

	for (y = 0; y < height; y++) {
		for (x = 0; x < XSIZE; x++) {
			ptr[x] = ctx->colors.background;
		}

		ptr += ctx->lpr;
	}
}

static void draw_grid(Context *ctx)
{
	int x, y;
    ULONG *ptr = ctx->base_address;

	int total_height = (ctx->features.net) ? 2 * YSIZE : YSIZE;

	// Horizontal lines
	for (y = 0; y < total_height; y += 10) {
		for (x = 0; x < XSIZE; x++) {
			ptr[x] = ctx->colors.grid;
		}
		ptr += 10 * ctx->lpr;
	}

	// Vertical lines
	for (x = 0; x < XSIZE; x += 60) {
		ptr = (ULONG *)ctx->base_address + x;

		for (y = 0; y < total_height; y++) {
			*ptr = ctx->colors.grid;
			ptr += ctx->lpr;
		}
	}
}

static void refresh_window(Context *ctx)
{
	clear(ctx);

	if (ctx->features.grid) {
		draw_grid(ctx);
	}

	if (ctx->features.public_mem) {
		plot(ctx, get_ptr(public_mem), ctx->colors.public_mem);
	}

	if (ctx->features.virtual_mem) {
		plot(ctx, get_ptr(virtual_mem), ctx->colors.virtual_mem);
	}

	if (ctx->features.video_mem) {
		plot(ctx, get_ptr(video_mem), ctx->colors.video_mem);
	}

	if (ctx->features.cpu) {
		plot(ctx, get_ptr(cpu), ctx->colors.cpu);
	}

	if (ctx->features.net) {
		plot_net(ctx);
	}

	BltBitMapRastPort(ctx->bm, 0, 0,
		ctx->window->RPort,
		ctx->window->BorderLeft,
		ctx->window->BorderTop,
		ctx->window->Width - (ctx->window->BorderRight + ctx->window->BorderLeft),
		ctx->window->Height - (ctx->window->BorderBottom + ctx->window->BorderTop),
		0xC0);

	snprintf(ctx->window_title, WINDOW_TITLE_LEN, WINDOW_TITLE_FORMAT,
		get_cur(cpu), get_cur(virtual_mem), get_cur(public_mem), get_cur(video_mem));

	snprintf(ctx->screen_title, SCREEN_TITLE_LEN, SCREEN_TITLE_FORMAT,
		get_cur(cpu), get_cur(virtual_mem), get_cur(public_mem), get_cur(video_mem),
		ctx->dl_speed, ctx->ul_speed, ctx->simple_mode ? 'S' : 'B');

	SetWindowTitles(ctx->window,
		(ctx->features.dragbar) ? ctx->window_title : NULL, ctx->screen_title);
}

static ULONG parse_hex(STRPTR str)
{
	return strtol(str, NULL, 16);
}
/*
static void set_bit(struct DiskObject *disk_object, STRPTR name, Context *ctx, int bit)
{
	STRPTR tool_type = FindToolType(disk_object->do_ToolTypes, name);
		
	if (tool_type) {
		//if (MatchToolValue(cpu, "True"))
		{
			ctx->show_bits |= bit;
		}
	}
}
*/
static void set_int(struct DiskObject *disk_object, STRPTR name, int *value)
{
	STRPTR tool_type = FindToolType(disk_object->do_ToolTypes, name);
		
	if (tool_type) {
		const int temp = atoi(tool_type);
			
		if (temp >= 0) {
			*value = temp;
		}
	}
}

static void set_bool(struct DiskObject *disk_object, STRPTR name, BOOL *value)
{
	STRPTR tool_type = FindToolType(disk_object->do_ToolTypes, name);
		
	if (tool_type) {
		*value = TRUE;
	}
}

static void set_color(struct DiskObject *disk_object, STRPTR name, ULONG *value)
{
	STRPTR tool_type = FindToolType(disk_object->do_ToolTypes, name);
		
	if (tool_type) {
		*value = parse_hex(tool_type);
	}
}

static void read_config(Context *ctx, STRPTR file_name)
{
	if (file_name) {

		struct DiskObject *disk_object = (struct DiskObject *)GetDiskObject(file_name);
			
		if (disk_object) {
			set_bool(disk_object, "cpu", &ctx->features.cpu);
			set_bool(disk_object, "grid", &ctx->features.grid);
			set_bool(disk_object, "pmem", &ctx->features.public_mem);
			set_bool(disk_object, "vmem", &ctx->features.virtual_mem);
			set_bool(disk_object, "gmem", &ctx->features.video_mem);
			set_bool(disk_object, "solid", &ctx->features.solid_draw);
			set_bool(disk_object, "transparent", &ctx->features.transparent);
			set_bool(disk_object, "dragbar", &ctx->features.dragbar);

			set_int(disk_object, "xpos", &ctx->x_pos);
			set_int(disk_object, "ypos", &ctx->y_pos);
				
			set_bool(disk_object, "simple", (BOOL *)&ctx->simple_mode);
				
			set_color(disk_object, "cpucol", &ctx->colors.cpu);
			set_color(disk_object, "bgcol", &ctx->colors.background);
			set_color(disk_object, "gmemcol", &ctx->colors.video_mem);
			set_color(disk_object, "pmemcol", &ctx->colors.public_mem);
			set_color(disk_object, "vmemcol", &ctx->colors.virtual_mem);
			set_color(disk_object, "gridcol", &ctx->colors.grid);
			set_color(disk_object, "ulcol", &ctx->colors.upload);
			set_color(disk_object, "dlcol", &ctx->colors.download);

			FreeDiskObject(disk_object);
		}
	}
}

static void handle_args(Context *ctx, int argc, char ** argv)
{
	if (! argc) {
		struct WBStartup *wb_startup = (struct WBStartup *) argv;
		struct WBArg *wb_arg = wb_startup->sm_ArgList;

		if (wb_arg /*&& wb_arg->wa_Lock*/) {
			//BPTR old_dir = GetCurrentDir(/*wb_arg->wa_Lock*/);

			read_config(ctx, wb_arg->wa_Name);

			//CurrentDir(old_dir);
		}
	} else {
		read_config(ctx, argv[0]);
	}
}

static void *my_alloc(size_t size)
{
	return AllocVecTags(size,
	 	AVT_ClearWithValue, 0,
		TAG_DONE);
}

static void my_free(void *ptr)
{
	FreeVec(ptr);
}

static struct Window *open_window(Context *ctx, int x, int y)
{
	return OpenWindowTags( NULL,
		//WA_Title, "CPU Watcher",
		WA_Left, x,
		WA_Top, y,
		WA_InnerWidth, XSIZE,
		WA_InnerHeight, (ctx->features.net) ? 2 * YSIZE : YSIZE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY /*| IDCMP_CHANGEWINDOW*/,
		WA_CloseGadget, (ctx->features.dragbar) ? TRUE : FALSE,
		WA_DragBar, (ctx->features.dragbar) ? TRUE : FALSE,
		WA_DepthGadget, (ctx->features.dragbar) ? TRUE : FALSE,
		WA_UserPort, ctx->user_port,
		TAG_DONE );
}

static BOOL allocate_resources(Context *ctx)
{
	BOOL result = FALSE;

	ctx->main_sig = AllocSignal(-1);

	if (ctx->main_sig == -1) {
		printf("Couldn't allocate signal\n");
		goto clean;
	}

	ctx->samples = my_alloc(XSIZE * sizeof(Sample));

	if (!ctx->samples) {
		printf("Couldn't allocate sample data\n");
		goto clean;
	}

	ctx->bm = AllocBitMapTags( XSIZE, 2 * YSIZE, 32,
		BMATags_PixelFormat, PIXF_A8R8G8B8,
		BMATags_Clear, TRUE,
		BMATags_UserPrivate, TRUE,
		TAG_DONE );
		
	if (!ctx->bm) {
		printf("Couln't allocate bitmap\n");
		goto clean;
	}

	APTR lock = LockBitMapTags( ctx->bm,
		LBM_BaseAddress, &ctx->base_address,
		LBM_BytesPerRow, &ctx->bytes_per_row,
		TAG_DONE );

	if (lock) {
		ctx->lpr = ctx->bytes_per_row / 4;
		UnlockBitMap(lock);
	} else {
		printf("Lock failed\n");
		goto clean;
	}

	ctx->user_port = AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Name, "user_port",
		TAG_DONE);

	if (!ctx->user_port) {
		printf("Couldn't create user port\n");
		goto clean;
	}

	ctx->timer_port = AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Name, "timer_port",
		TAG_DONE);

	if (!ctx->timer_port) {
		printf("Couldn't create timer port\n");
		goto clean;
	}

	ctx->timer_req = AllocSysObjectTags(ASOT_IOREQUEST,
		ASOIOR_Size, sizeof(struct TimeRequest),
		ASOIOR_ReplyPort, ctx->timer_port,
		TAG_DONE);

	if (!ctx->timer_req) {
		printf("Couldn't create IO request\n");
		goto clean;
	}

	ctx->timer_device = OpenDevice(TIMERNAME, UNIT_WAITUNTIL,
		(struct IORequest *) ctx->timer_req, 0);
		
	if (ctx->timer_device) {		
		printf("Couldn't open timer.device\n");
		goto clean;
	}

	ITimer = (struct TimerIFace *) GetInterface(
	    (struct Library *) ctx->timer_req->Request.io_Device, "main", 1, NULL);
		
	if (!ITimer) {
		printf("Couldn't get Timer interface\n");
		goto clean;
	}

	ctx->window = open_window(ctx, ctx->x_pos, ctx->y_pos);

	if (!ctx->window) {
		printf("Couldn't open window\n");
		goto clean;
	}
		
	ctx->idle_task = CreateTaskTags("Uuno", 0, idler, 4096,
		AT_Param1, ctx,
		TAG_DONE);

	if (!ctx->idle_task) {
		printf("Couldn't create idler task\n");
		goto clean;
	}

	ctx->window_title = my_alloc(WINDOW_TITLE_LEN);

	if (!ctx->window_title) {
		printf("Couldn't allocate window title\n");
		goto clean;
	}

	ctx->screen_title = my_alloc(SCREEN_TITLE_LEN);

	if (!ctx->screen_title) {
		printf("Couldn't allocate screen title\n");
		goto clean;
	}

	result = TRUE;

clean:

	return result;
}

static void handle_keyboard(Context *ctx, UWORD key)
{
	BOOL update = TRUE;

	switch (key) {
		case 'c':
			ctx->features.cpu ^= TRUE;
			break;

		case 'p':
			ctx->features.public_mem ^= TRUE;
			break;

		case 'v':
			ctx->features.virtual_mem ^= TRUE;
			break;

		case 'x':
			ctx->features.video_mem ^= TRUE;
			break;

		case 'g':
			ctx->features.grid ^= TRUE;
			break;

		case 's':
			ctx->features.solid_draw ^= TRUE;
			break;

		case 't':
			ctx->features.transparent ^= TRUE;
			if (ctx->features.transparent) {
				//TODO
			}
			break;

		case 'm':
			ctx->simple_mode ^= TRUE;
			break;

		case 'n':
			ctx->features.net ^= TRUE;
			SizeWindow(ctx->window, 0, (ctx->features.net) ? YSIZE : -YSIZE);
			break;

		case 'd':
			ctx->features.dragbar ^= TRUE;

			// Store old coordinates
			const WORD x = ctx->window->LeftEdge;
			const WORD y = ctx->window->TopEdge;

			// Window is using WA_UserPort, so CloseWindow() will do CloseWindowSafely()
			CloseWindow(ctx->window);

			ctx->window = open_window(ctx, x, y);

			if (ctx->window) {
				ActivateWindow(ctx->window);
			} else {
				printf("Panic - can't reopen window!\n");
				ctx->running = FALSE;
			}
			break;

		case 'q':
			ctx->running = FALSE;
			break;

		default:
			update = FALSE;
			break;
	}

	if (update) {
		refresh_window(ctx);
    }
}

static void handle_window_events(Context *ctx)
{
	struct IntuiMessage *msg;
	while ((msg = (struct IntuiMessage *) GetMsg(ctx->window->UserPort)))
	{
		const ULONG event_class = msg->Class;
		const UWORD event_code = msg->Code;

		ReplyMsg((struct Message *) msg);

		if (event_class == IDCMP_CLOSEWINDOW) {
			ctx->running = FALSE;
		} else if (event_class == IDCMP_VANILLAKEY) {
			handle_keyboard(ctx, event_code);
		}
	}
}

static UBYTE clamp100(UBYTE value)
{
	if (value > 100) {
		//printf("%d\n", value);
		value = 100;
	}

	return value;
}

static void measure_cpu(Context *ctx)
{
	UBYTE value = 100;

	if (ctx->simple_mode) {
		value -= ctx->run_count;
	} else {
		value -= 100 * (idle_time.total.Seconds * 1000000 + idle_time.total.Microseconds) / 1000000.0f;
	}

	ctx->run_count = 0;
	idle_time.total.Seconds = 0;
	idle_time.total.Microseconds = 0;

	ctx->samples[ctx->iter].cpu = clamp100(value);
}

static void measure_memory(Context *ctx)
{
	UBYTE value = 100 * (float) AvailMem(MEMF_PUBLIC) / AvailMem(MEMF_PUBLIC|MEMF_TOTAL);

	ctx->samples[ctx->iter].public_mem = clamp100(value);

	value = 100 * (float) AvailMem(MEMF_VIRTUAL) / AvailMem(MEMF_VIRTUAL|MEMF_TOTAL);

	ctx->samples[ctx->iter].virtual_mem = clamp100(value);

	uint64 total_vid, free_vid;

	if (GetBoardDataTags(0,
		GBD_TotalMemory, &total_vid,
		GBD_FreeMemory, &free_vid,
		TAG_DONE) == 2) {
			
		value = 100.0 * free_vid / total_vid;

		ctx->samples[ctx->iter].video_mem = clamp100(value);
	}
}

static void measure_network(Context *ctx, float *dl_speed, float *ul_speed)
{
	float dl_mult = 1.0f, ul_mult = 1.0f;
	UBYTE dl_p, ul_p;

	if (update_netstats(&dl_p, &ul_p, &dl_mult, &ul_mult, dl_speed, ul_speed)) {
			
		int i;
		for (i = 0; i < XSIZE; i++) {
			ctx->samples[i].download *= dl_mult;
			ctx->samples[i].upload *= ul_mult;
		}
	}

	ctx->samples[ctx->iter].download = dl_p;
	ctx->samples[ctx->iter].upload = ul_p;
}

static void start_timer(Context *ctx)
{
	struct TimeVal increment;

	GetSysTime(&ctx->tv);

	increment.Seconds = 1;
	increment.Microseconds = 0;

	AddTime(&ctx->tv, &increment);

	ctx->timer_req->Request.io_Command = TR_ADDREQUEST;
	ctx->timer_req->Time.Seconds = ctx->tv.Seconds;
	ctx->timer_req->Time.Microseconds = ctx->tv.Microseconds;

	SendIO((struct IORequest *) ctx->timer_req);
}

static void handle_timer_events(Context *ctx)
{
	GetMsg(ctx->timer_port);

	start_timer(ctx);

	// Timer signal, update visuals once per second
	//if ( (++count % FREQ) == 0)
	{
		++ctx->iter;
		ctx->iter %= XSIZE;

		measure_cpu(ctx);
		measure_memory(ctx);
		measure_network(ctx, &ctx->dl_speed, &ctx->ul_speed);

		refresh_window(ctx);
   	}
}

static void stop_timer(Context *ctx)
{
	if (!CheckIO((struct IORequest *) ctx->timer_req)) {
		AbortIO((struct IORequest *) ctx->timer_req);
		WaitIO((struct IORequest *) ctx->timer_req);
	}
}

static void wait_for_idler(Context *ctx)
{
	// if idler task had problems, don't wait for it
	if (ctx->idle_task && !ctx->idler_trouble) {
	    // Give it some more cpu
		SetTaskPri(ctx->idle_task, 0);

	 	// Wait idler task to finish possible timer actions before closing timing services
		Wait(1L << ctx->main_sig | SIGBREAKF_CTRL_C);
	}
}

static void free_resources(Context *ctx)
{
	wait_for_idler(ctx);

	if (ITimer) {
		DropInterface((struct Interface *) ITimer);
	}

	if (ctx->timer_device == 0 && ctx->timer_req) {
		CloseDevice((struct IORequest *) ctx->timer_req);
	}

	if (ctx->timer_req) {
		FreeSysObject(ASOT_IOREQUEST, ctx->timer_req);
	}

	if (ctx->timer_port) {
		FreeSysObject(ASOT_PORT, ctx->timer_port);
	}

	if (ctx->idle_task) {
		DeleteTask(ctx->idle_task);
	}

	if (ctx->main_sig != -1) {
		FreeSignal(ctx->main_sig);
	}

	if (ctx->window) {
		CloseWindow(ctx->window);
	}

	if (ctx->user_port) {
		FreeSysObject(ASOT_PORT, ctx->user_port);
	}

	if (ctx->bm) {
		FreeBitMap(ctx->bm);
	}

	if (ctx->window_title) my_free(ctx->window_title);
	if (ctx->screen_title) my_free(ctx->screen_title);

	if (ctx->samples) my_free(ctx->samples);
}

static void init_context(Context *ctx)
{
	memset(ctx, 0, sizeof(Context));

	ctx->main_task = FindTask(NULL);

	ctx->main_sig = -1;
	ctx->idle_sig = -1;

	ctx->features.cpu = TRUE;
	ctx->features.public_mem = TRUE;
	ctx->features.virtual_mem = TRUE;
	ctx->features.video_mem = TRUE;
	ctx->features.solid_draw = TRUE;
	ctx->features.dragbar = TRUE;
	ctx->features.grid = TRUE;

	ctx->running = TRUE;
	ctx->timer_device = -1;

	ctx->colors.cpu = CPU_COL;
	ctx->colors.public_mem = PUB_COL;
	ctx->colors.virtual_mem = VIRT_COL;
	ctx->colors.video_mem = VID_COL;
	ctx->colors.grid = GRID_COL;
	ctx->colors.background = BG_COL;
	ctx->colors.upload = UL_COL;
	ctx->colors.download = DL_COL;
}

static void main_loop(Context *ctx)
{
	while ( ctx->running ) {
		ULONG sigs = Wait(
			SIGBREAKF_CTRL_C |
			1L << ctx->timer_port->mp_SigBit |
			1L << ctx->window->UserPort->mp_SigBit);

		if (sigs & (1L << ctx->timer_port->mp_SigBit)) {
			handle_timer_events(ctx);
		}

		if (sigs & 1L << ctx->window->UserPort->mp_SigBit) {
			handle_window_events(ctx);
		}

		if (sigs & SIGBREAKF_CTRL_C) {
			ctx->running = FALSE;
		}
	}
}

static BOOL sync_to_idler_task(Context *ctx)
{
	BOOL result = FALSE;

	Wait(1L << ctx->main_sig);

	if (ctx->idler_trouble) {
		goto clean;
	}

	Signal(ctx->idle_task, 1L << ctx->idle_sig);

	result = TRUE;

clean:
	return result;
}

int main(int argc, char ** argv)
{
	Context ctx;
	init_context(&ctx);

	//printf("Quantum %d\n", ((struct ExecBase *)SysBase)->Quantum);

	if (GfxBase->lib_Version < 54) {
		printf("graphics.library V54 needed\n");
	} else {
		handle_args(&ctx, argc, argv);

		if (allocate_resources(&ctx) && sync_to_idler_task(&ctx)) {

			refresh_window(&ctx);

			init_netstats();

			start_timer(&ctx);

			main_loop(&ctx);

			stop_timer(&ctx);
		}
	}

	free_resources(&ctx);

	return 0;
}

