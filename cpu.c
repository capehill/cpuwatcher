/*

CPU Watcher 0.7 by Juha Niemimäki (c) 2005 - 2020

- measures CPU, free memory and network traffic.


Special thanks to Thomas Frieden, Olaf Barthel, Alex Carmona and Dave Fisher.

TODO:
- prefs editor

*/

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/dos.h>
#include <proto/keymap.h>

#include <dos/dos.h>
#include <workbench/startup.h>
#include <intuition/menuclass.h>
#include <classes/requester.h>
#include <classes/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VERSION_STRING "CPU Watcher 0.7"
#define DATE_STRING " (11.3.2020)"

static __attribute__((used)) char *version_string = "$VER: " VERSION_STRING DATE_STRING;

#define WINDOW_TITLE_FORMAT "CPU: %3d%% RAM: %3d%% VID: %3d%%"
#define SCREEN_TITLE_FORMAT \
"CPU load: %3d%%. Free memory: %3d%%. Free video memory: %3d%%. Download: %4.1fKiB/s. Upload: %4.1fKiB/s. Mode: %s"

#define WINDOW_TITLE_LEN 64
#define SCREEN_TITLE_LEN 128

#define MINUTES 5
#define XSIZE (60 * MINUTES)

// 0...100 %
#define YSIZE 101

// Graph colors
#define CPU_COL		0xFF00A000 // Green
#define VIRT_COL	0xFF1010FF // Blue
#define VID_COL		0xFF10C0F0 // Brighter blue
#define GRID_COL	0xFF003000 // Dark green
#define DL_COL		0xFF00A000 // Green
#define UL_COL		0xFFFF1010 // Red
#define BG_COL		0xFF000000

#define MAX_OPAQUENESS 255
#define MIN_OPAQUENESS 20

extern struct Library *GfxBase;
struct TimerIFace *ITimer = NULL;

typedef struct {
	BOOL cpu;
	BOOL grid;
	BOOL virtual_mem;
	BOOL video_mem;
	BOOL solid_draw;
	BOOL net;
	BOOL dragbar;
	BOOL resize;
} Features;

typedef struct {
	ULONG cpu;
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
	struct RastPort rastPort;

	Object* windowObject;

	ULONG width;
	ULONG height;

	float scaleX;
	float scaleY;

	struct MsgPort *timer_port;
	struct MsgPort *user_port;
	struct MsgPort *app_port;

	struct Task *main_task;
	struct Task *idle_task;

	BYTE idle_sig;
	BYTE main_sig;

	struct TimeRequest *timer_req;
		
	BYTE timer_device;
	struct TimeVal tv;
		
	int x_pos;
	int y_pos;

	UBYTE opaqueness;

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

#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define SCALE_X(x) (int)roundf((float)(x) * ctx->scaleX)
#define SCALE_Y(y) (int)roundf((float)(y) * ctx->scaleY)

typedef enum EMenu {
    MID_Iconify = 1,
    MID_About,
    MID_Quit
} EMenu;

Object* menu;

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

static void idle_sleep(struct TimeRequest *pause_req)
{
	struct TimeVal dest, source;
	BYTE error;

	GetSysTime(&dest);

	source.Seconds = 0;
	source.Microseconds = 10000;

	AddTime(&dest, &source);

	pause_req->Request.io_Command = TR_ADDREQUEST;
	pause_req->Time.Seconds = dest.Seconds;
	pause_req->Time.Microseconds = dest.Microseconds;

	error = DoIO((struct IORequest *) pause_req);

	if (error) {
		DebugPrintF("DoIO returned %d\n", error);
	}
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
			ctx->run_count++;
			idle_sleep(pause_req);
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

#if 0
static void point(Context *ctx, int x, int y, ULONG color)
{
	WritePixelColor(&ctx->rastPort, x, y, color);
}

static void line(Context *ctx, int sx, int sy, int fx, int fy, ULONG color)
{
	Move(&ctx->rastPort, sx, sy);
	SetRPAttrs(&ctx->rastPort, RPTAG_APenColor, color, TAG_DONE);
	Draw(&ctx->rastPort, fx, fy);
}
#endif

static void vertical_line(Context *ctx, int x, int start, int end, ULONG color)
{
	Move(&ctx->rastPort, x, start);
	SetRPAttrs(&ctx->rastPort, RPTAG_APenColor, color, TAG_DONE);
	Draw(&ctx->rastPort, x, end);
}

static void horizontal_line(Context *ctx, int y, int start, int end, ULONG color)
{
	Move(&ctx->rastPort, start, y);
	SetRPAttrs(&ctx->rastPort, RPTAG_APenColor, color, TAG_DONE);
	Draw(&ctx->rastPort, end, y);
}

static void line_to(Context *ctx, int x, int y, ULONG color)
{
	SetRPAttrs(&ctx->rastPort, RPTAG_APenColor, color, TAG_DONE);
	Draw(&ctx->rastPort, x, y);
}

static void plot(Context *ctx, const UBYTE* const array, const ULONG color)
{
	int	x;
	for (x = 0; x < XSIZE; x++) {
		const int iter = (ctx->iter + 1 + x) % XSIZE;
		const int level = *(array + iter * sizeof(Sample));
		const int y = YSIZE - level;

		if (x == 0) {
			Move(&ctx->rastPort, 0, SCALE_Y(y) - 1);
		} else {
			line_to(ctx, SCALE_X(x), SCALE_Y(y) - 1, color);
		}
	}
}

static void plot_net(Context *ctx, UBYTE *array, const int bottom, const ULONG color)
{
	int x;
	for (x = 0; x < XSIZE; x++) {
		const int iter = (ctx->iter + 1 + x) % XSIZE;
		const int level = *(array + iter * sizeof(Sample)) / 2;
		const int start = bottom - level;

		if (x == 0) {
			Move(&ctx->rastPort, 0, SCALE_Y(start) - 1);
		} else {
			line_to(ctx, SCALE_X(x), SCALE_Y(start) - 1, color);
		}
	}
}

static void clear(Context *ctx)
{
	RectFillColor(&ctx->rastPort, 0, 0, ctx->width - 1, ctx->height - 1, ctx->colors.background);
}

static void draw_grid(Context *ctx)
{
	float y;
	const float step = (ctx->features.net) ? ctx->height / 20.f : ctx->height / 10.f;

	for (y = 0; y < ctx->height; y += step) {
		horizontal_line(ctx, y, 0, ctx->width - 1, ctx->colors.grid);
	}

	float x;
	for (x = 0; x < ctx->width; x+= ctx->width / 5.f) {
		vertical_line(ctx, x, 0, ctx->height - 1, ctx->colors.grid);
	}
}

static void refresh_window(Context *ctx)
{
	clear(ctx);

	if (ctx->features.grid) {
		draw_grid(ctx);
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
		plot_net(ctx, get_ptr(upload), YSIZE + YSIZE / 2, ctx->colors.upload);
		plot_net(ctx, get_ptr(download), 2 * YSIZE, ctx->colors.download);
	}

	BltBitMapRastPort(ctx->bm, 0, 0,
		ctx->window->RPort,
		ctx->window->BorderLeft,
		ctx->window->BorderTop,
		ctx->window->Width - (ctx->window->BorderRight + ctx->window->BorderLeft),
		ctx->window->Height - (ctx->window->BorderBottom + ctx->window->BorderTop),
		0xC0);

	snprintf(ctx->window_title, WINDOW_TITLE_LEN, WINDOW_TITLE_FORMAT,
		get_cur(cpu), get_cur(virtual_mem), get_cur(video_mem));

	snprintf(ctx->screen_title, SCREEN_TITLE_LEN, SCREEN_TITLE_FORMAT,
		get_cur(cpu), get_cur(virtual_mem), get_cur(video_mem),
		ctx->dl_speed, ctx->ul_speed, ctx->simple_mode ? "Simple" : "Busy");

	SetWindowTitles(ctx->window,
		(ctx->features.dragbar) ? ctx->window_title : NULL, ctx->screen_title);
}

static ULONG parse_hex(STRPTR str)
{
	return strtol(str, NULL, 16);
}

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

	*value = (tool_type) ? TRUE : FALSE;
}

static void set_color(struct DiskObject *disk_object, STRPTR name, ULONG *value)
{
	STRPTR tool_type = FindToolType(disk_object->do_ToolTypes, name);
		
	if (tool_type) {
		*value = parse_hex(tool_type);
	}
}

static UBYTE validate_opaqueness(int opaqueness)
{
	if (opaqueness > MAX_OPAQUENESS) {
		opaqueness = MAX_OPAQUENESS;
	} else if (opaqueness < MIN_OPAQUENESS) {
		opaqueness = MIN_OPAQUENESS;
	}

	return opaqueness;
}

static void read_config(Context *ctx, STRPTR file_name)
{
	if (file_name) {

		struct DiskObject *disk_object = (struct DiskObject *)GetDiskObject(file_name);
			
		if (disk_object) {
			int opaqueness = 255;

			set_bool(disk_object, "cpu", &ctx->features.cpu);
			set_bool(disk_object, "grid", &ctx->features.grid);
			set_bool(disk_object, "vmem", &ctx->features.virtual_mem);
			set_bool(disk_object, "gmem", &ctx->features.video_mem);
			set_bool(disk_object, "solid", &ctx->features.solid_draw);
			set_bool(disk_object, "dragbar", &ctx->features.dragbar);
			set_bool(disk_object, "net", &ctx->features.net);
			set_bool(disk_object, "simple", (BOOL *)&ctx->simple_mode);
			set_bool(disk_object, "resize", &ctx->features.resize);

			set_int(disk_object, "xpos", &ctx->x_pos);
			set_int(disk_object, "ypos", &ctx->y_pos);
			//set_int(disk_object, "width", &ctx->width); TODO?
			//set_int(disk_object, "height", &ctx->height);
			set_int(disk_object, "opaqueness", &opaqueness);

			ctx->opaqueness = validate_opaqueness(opaqueness);
				
			set_color(disk_object, "cpucol", &ctx->colors.cpu);
			set_color(disk_object, "bgcol", &ctx->colors.background);
			set_color(disk_object, "gmemcol", &ctx->colors.video_mem);
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
			read_config(ctx, wb_arg->wa_Name);
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

static Object* create_menu()
{
    menu = NewObject(NULL, "menuclass",
        MA_Type, T_ROOT,
        // Main
        MA_AddChild, NewObject(NULL, "menuclass",
            MA_Type, T_MENU,
            MA_Label, "Main",
            MA_AddChild, NewObject(NULL, "menuclass",
                MA_Type, T_ITEM,
                MA_Label, "About",
                MA_ID, MID_About,
                TAG_DONE),
            MA_AddChild, NewObject(NULL, "menuclass",
                MA_Type, T_ITEM,
                MA_Label, "Iconfiy",
                MA_ID, MID_Iconify,
                TAG_DONE),
            MA_AddChild, NewObject(NULL, "menuclass",
                MA_Type, T_ITEM,
                MA_Label, "Quit",
                MA_ID, MID_Quit,
                TAG_DONE),
            TAG_DONE),
        TAG_DONE);

    if (!menu) {
        puts("Failed to create menu");
    }

    return menu;
}

static char* getApplicationName()
{
    #define maxPathLen 255

    static char pathBuffer[maxPathLen];

	if (!GetCliProgramName(pathBuffer, maxPathLen - 1)) {
		puts("Failed to get CLI program name, checking task node");
		snprintf(pathBuffer, maxPathLen, "%s", ((struct Node *)FindTask(NULL))->ln_Name);
    }

    return pathBuffer;
}

static struct DiskObject* getDiskObject()
{
    struct DiskObject *diskObject = NULL;

	BPTR oldDir = SetCurrentDir(GetProgramDir());
	diskObject = GetDiskObject(getApplicationName());
	SetCurrentDir(oldDir);

    return diskObject;
}

static struct Window *open_window(Context *ctx, int x, int y)
{
	const int minWidth = XSIZE;
	const int minHeight = (ctx->features.net) ? 2 * YSIZE : YSIZE;

	int width = minWidth;
	int height = minHeight;

	if (ctx->windowObject) {
		width = ctx->width;
		height = ctx->height;

		DisposeObject(ctx->windowObject);
		ctx->windowObject = NULL;
		ctx->window = NULL;
	}

	 ctx->windowObject = NewObject(NULL, "window.class",
        WA_Activate, TRUE,
		WA_Left, x,
		WA_Top, y,
		WA_InnerWidth, width,
		WA_InnerHeight, height,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_NEWSIZE | IDCMP_MENUPICK,
		WA_CloseGadget, ctx->features.dragbar,
		WA_DragBar, ctx->features.dragbar,
		WA_DepthGadget, ctx->features.dragbar,
		WA_SizeGadget, ctx->features.resize,
		WA_UserPort, ctx->user_port,
		WA_Opaqueness, ctx->opaqueness,
		WA_MenuStrip, create_menu(),
        WINDOW_IconifyGadget, TRUE,
		WINDOW_Icon, getDiskObject(),
		WINDOW_AppPort, ctx->app_port, // Iconification needs it
		TAG_DONE);

	if (!ctx->windowObject) {
		puts("Failed to create window object");
		return NULL;
	}
	
	struct Window* window = (struct Window *)IDoMethod(ctx->windowObject, WM_OPEN);

	if (!window) {
		puts("Failed to open window");
		return NULL;
	}

	if (!WindowLimits(window, minWidth + window->BorderLeft + window->BorderRight,
		minHeight + window->BorderTop + window->BorderBottom, 1024, 1024)) {
		puts("Failed to set window limits");
	}

	return window;
}

static void query_window_size(Context *ctx)
{
	if ((GetWindowAttrs(ctx->window,
		WA_InnerWidth, &ctx->width,
		WA_InnerHeight, &ctx->height,
		TAG_DONE)) != 0)
	{
			puts("Failed get window attributes");
	}

	ctx->scaleX = (float)ctx->width / (float)XSIZE;
	ctx->scaleY = (float)ctx->height / (ctx->features.net ? 2.0f * (float)YSIZE : (float)YSIZE);
}

static BOOL realloc_bitmap(Context *ctx)
{
	ULONG w = 0;
	ULONG h = 0;

	if (ctx->bm) {
		w = GetBitMapAttr(ctx->bm, BMA_ACTUALWIDTH);
		h = GetBitMapAttr(ctx->bm, BMA_HEIGHT);
	}

	query_window_size(ctx);

	if (!ctx->bm || w < ctx->width || h < ctx->height) {
		if (ctx->bm) {
    		FreeBitMap(ctx->bm);
		}

		ctx->bm = AllocBitMapTags(ctx->width, ctx->height, 32,
    		BMATags_PixelFormat, PIXF_A8R8G8B8,
    		BMATags_Clear, TRUE,
#if 0 // There doesn't seem to be much difference whether bitmap is in RAM or VRAM
            BMATags_Displayable, TRUE,
#else
    		BMATags_UserPrivate, TRUE,
#endif
    		TAG_DONE);

    	if (!ctx->bm) {
    		puts("Couldn't allocate bitmap");
    		return FALSE;
    	}

    	InitRastPort(&ctx->rastPort);
    	ctx->rastPort.BitMap = ctx->bm;
    }

    return TRUE;
}

static BOOL allocate_resources(Context *ctx)
{
	BOOL result = FALSE;

	ctx->main_sig = AllocSignal(-1);

	if (ctx->main_sig == -1) {
		puts("Couldn't allocate signal");
		goto clean;
	}

	ctx->samples = my_alloc(XSIZE * sizeof(Sample));

	if (!ctx->samples) {
		puts("Couldn't allocate sample data");
		goto clean;
	}

	ctx->user_port = AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Name, "user_port",
		TAG_DONE);

	if (!ctx->user_port) {
		puts("Couldn't create user port");
		goto clean;
	}

	ctx->timer_port = AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Name, "timer_port",
		TAG_DONE);

	if (!ctx->timer_port) {
		puts("Couldn't create timer port");
		goto clean;
	}

	ctx->app_port = AllocSysObjectTags(ASOT_PORT,
		ASOPORT_Name, "app_port",
		TAG_DONE);

	if (!ctx->app_port) {
		puts("Couldn't create app port");
		goto clean;
	}

	ctx->timer_req = AllocSysObjectTags(ASOT_IOREQUEST,
		ASOIOR_Size, sizeof(struct TimeRequest),
		ASOIOR_ReplyPort, ctx->timer_port,
		TAG_DONE);

	if (!ctx->timer_req) {
		puts("Couldn't create IO request");
		goto clean;
	}

	ctx->timer_device = OpenDevice(TIMERNAME, UNIT_WAITUNTIL,
		(struct IORequest *) ctx->timer_req, 0);
		
	if (ctx->timer_device) {		
		puts("Couldn't open timer.device");
		goto clean;
	}

	ITimer = (struct TimerIFace *) GetInterface(
		(struct Library *) ctx->timer_req->Request.io_Device, "main", 1, NULL);
		
	if (!ITimer) {
		puts("Couldn't get Timer interface");
		goto clean;
	}

	ctx->window = open_window(ctx, ctx->x_pos, ctx->y_pos);

	if (!ctx->window) {
		puts("Couldn't open window");
		goto clean;
	}

	ctx->window_title = my_alloc(WINDOW_TITLE_LEN);

	if (!ctx->window_title) {
		puts("Couldn't allocate window title");
		goto clean;
	}

	ctx->screen_title = my_alloc(SCREEN_TITLE_LEN);

	if (!ctx->screen_title) {
		puts("Couldn't allocate screen title");
		goto clean;
	}

    realloc_bitmap(ctx);

	if (!ctx->bm) {
		puts("Couln't allocate bitmap");
		goto clean;
	}

	ctx->idle_task = CreateTaskTags("Uuno", 0, idler, 4096,
		AT_Param1, ctx,
		TAG_DONE);

	if (!ctx->idle_task) {
		puts("Couldn't create idler task");
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

		case 'm':
			ctx->simple_mode ^= TRUE;
			break;

		case 'n':
			ctx->features.net ^= TRUE;
			SizeWindow(ctx->window, 0, (ctx->features.net) ? ctx->height : -ctx->height / 2);
			refresh_window(ctx);
			break;

		case 'd':
			ctx->features.dragbar ^= TRUE;

			// Remember old coordinates
			const WORD x = ctx->window->LeftEdge;
			const WORD y = ctx->window->TopEdge;

			ctx->window = open_window(ctx, x, y);

			if (ctx->window) {
				ActivateWindow(ctx->window);
			} else {
				puts("Panic - can't reopen window!");
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

static void show_about_window(Context* ctx)
{
    Object* o = NewObject(NULL, "requester.class",
        REQ_TitleText, "About CPU Watcher",
        REQ_BodyText, VERSION_STRING DATE_STRING,
        REQ_GadgetText, "_Ok",
        REQ_Image, REQIMAGE_INFO,
        TAG_DONE);

    if (o) {
        SetWindowPointer(ctx->window, WA_BusyPointer, TRUE, TAG_DONE);
        IDoMethod(o, RM_OPENREQ, NULL, ctx->window, NULL, TAG_DONE);
        SetWindowPointer(ctx->window, TAG_DONE);
        DisposeObject(o);
    }
}

static void handle_iconify(Context* ctx)
{
	ctx->window = NULL;
	IDoMethod(ctx->windowObject, WM_ICONIFY);
}

static void handle_uniconify(Context* ctx)
{
	ctx->window = (struct Window *)IDoMethod(ctx->windowObject, WM_OPEN);
}

static BOOL handle_menupick(Context *ctx)
{
    BOOL running = TRUE;

    uint32 id = NO_MENU_ID;

	while (ctx->window && ((id = IDoMethod((Object *)ctx->window->MenuStrip, MM_NEXTSELECT, 0, id))) != NO_MENU_ID) {
        switch(id) {
            case MID_Quit:
                running = FALSE;
                break;
            case MID_Iconify:
				handle_iconify(ctx);
                break;
            case MID_About:
                show_about_window(ctx);
                break;
        }
    }

    return running;
}

static char getVanillaKey(Context *ctx)
{
	struct InputEvent *ie;
	char vanilla;

	if (!GetAttr(WINDOW_InputEvent, ctx->windowObject, (ULONG *)&ie)) {
		puts("GetAttr failed");
		return 0;
	}

	if (MapRawKey(ie, &vanilla, 1, NULL) == 1) {
		return vanilla;
	}

	return 0;
}

static void handle_window_events(Context *ctx)
{
    uint32 result;
    int16 code = 0;

	while ((result = IDoMethod(ctx->windowObject, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG) {
        switch (result & WMHI_CLASSMASK) {
            case WMHI_CLOSEWINDOW:
				ctx->running = FALSE;
                break;
#if 0 // Doesn't trigger?
			case WMHI_VANILLAKEY:
				handle_keyboard(ctx, result & WMHI_KEYMASK);
				break;
#endif
			case WMHI_RAWKEY:
				handle_keyboard(ctx, getVanillaKey(ctx));
				break;
			case WMHI_NEWSIZE:
				realloc_bitmap(ctx);
				refresh_window(ctx);
				break;
            case WMHI_ICONIFY:
				handle_iconify(ctx);
                break;
            case WMHI_UNICONIFY:
				handle_uniconify(ctx);
                break;
            case WMHI_MENUPICK:
				ctx->running = handle_menupick(ctx);
                break;
			default:
				//printf("unhandled result %lx, code %x\n", result, code);
				break;
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
		value -= roundf(100.0f * (idle_time.total.Seconds * 1000000 + idle_time.total.Microseconds) / 1000000.0f);
	}

	ctx->run_count = 0;
	idle_time.total.Seconds = 0;
	idle_time.total.Microseconds = 0;

	ctx->samples[ctx->iter].cpu = clamp100(value);
}

static void measure_memory(Context *ctx)
{
	UBYTE value = roundf(100.0f * (float)AvailMem(MEMF_VIRTUAL) / (float)AvailMem(MEMF_VIRTUAL|MEMF_TOTAL));

	ctx->samples[ctx->iter].virtual_mem = clamp100(value);

	uint64 total_vid, free_vid;

	if (GetBoardDataTags(0,
		GBD_TotalMemory, &total_vid,
		GBD_FreeMemory, &free_vid,
		TAG_DONE) == 2) {
			
		value = roundf(100.0f * (float)free_vid / (float)total_vid);

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
	struct Message *msg;

	while ((msg = GetMsg(ctx->timer_port))) {
		int8 error = ((struct IORequest *)msg)->io_Error;

		if (error) {
			printf("message received with code %d\n", error);
		}
	}

	start_timer(ctx);

	++ctx->iter;
	ctx->iter %= XSIZE;

	measure_cpu(ctx);
	measure_memory(ctx);
	measure_network(ctx, &ctx->dl_speed, &ctx->ul_speed);

	if (ctx->window) {
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


	if (ctx->windowObject) {
		DisposeObject(ctx->windowObject);
	}

	if (ctx->user_port) {
		FreeSysObject(ASOT_PORT, ctx->user_port);
	}

	if (ctx->app_port) {
		FreeSysObject(ASOT_PORT, ctx->app_port);
	}

	if (ctx->bm) {
		FreeBitMap(ctx->bm);
	}

	if (ctx->window_title) {
		my_free(ctx->window_title);
	}

	if (ctx->screen_title) {
		my_free(ctx->screen_title);
	}

	if (ctx->samples) {
		my_free(ctx->samples);
	}
}

static void init_context(Context *ctx)
{
	memset(ctx, 0, sizeof(Context));

	ctx->main_task = FindTask(NULL);

	ctx->main_sig = -1;
	ctx->idle_sig = -1;

	ctx->features.cpu = TRUE;
	ctx->features.virtual_mem = TRUE;
	ctx->features.video_mem = TRUE;
	ctx->features.solid_draw = TRUE;
	ctx->features.dragbar = TRUE;
	ctx->features.grid = TRUE;
	ctx->features.resize = TRUE;

	ctx->running = TRUE;
	ctx->timer_device = -1;

	ctx->colors.cpu = CPU_COL;
	ctx->colors.virtual_mem = VIRT_COL;
	ctx->colors.video_mem = VID_COL;
	ctx->colors.grid = GRID_COL;
	ctx->colors.background = BG_COL;
	ctx->colors.upload = UL_COL;
	ctx->colors.download = DL_COL;

	ctx->opaqueness = 255;

	ctx->scaleX = 1.0f;
	ctx->scaleY = 1.0f;
}

static void main_loop(Context *ctx)
{
	while ( ctx->running ) {
		uint32 winSig = 0;
		if (!GetAttr(WINDOW_SigMask, ctx->windowObject, &winSig)) {
			puts("GetAttr failed");
		}

		const ULONG sigs = Wait(SIGBREAKF_CTRL_C | 1L << ctx->timer_port->mp_SigBit | winSig);

		if (sigs & (1L << ctx->timer_port->mp_SigBit)) {
			handle_timer_events(ctx);
		}

		if (sigs & winSig) {
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
	static Context ctx;

	init_context(&ctx);

	if (GfxBase->lib_Version < 54) {
		puts("graphics.library V54 needed");
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

