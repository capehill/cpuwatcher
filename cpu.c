/*

CPU Watcher 0.6 by Juha Niemimäki (c) 2005 - 2016

- measures CPU, free memory and network traffic.


Special thanks to Thomas Frieden, Olaf Barthel, Alex Carmona and Dave Fisher.

TODO:
- replace deprecated functions
- tooltype support
- prefs editor

Change log:

0.6
- fixed uninitialized ITimer issue
- code cleanup and refactoring

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
#include <proto/picasso96api.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/icon.h>
#include <proto/dos.h>

#include <dos/dos.h>
#include <workbench/startup.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __attribute__((used)) char *version_string = "$VER: CPU Watcher 0.6 (28.7.2016)";

#define MINUTES 5
#define XSIZE (60 * MINUTES)

// 0...100 %
#define YSIZE 101

// Graph colors
#define CPU_COL 	0x0000A000 // Green
#define PUB_COL 	0x00FF1010 // Red
#define VIRT_COL	0x001010FF // Blue
#define GFX_COL 	0x0010C0F0 // Brighter blue
#define GRID_COL 	0x00003000 // Dark green
#define DL_COL      0x0000A000 // Green
#define UL_COL      0x00FF1010 // Red
#define BG_COL		0x00000000

#define SHOW_CPU 	(1L << 0) // CPU
#define SHOW_GRID 	(1L << 1) // Grid
#define SHOW_PMEM 	(1L << 2) // Public M
#define SHOW_VMEM   (1L << 3) // Virtual M
#define SHOW_GMEM   (1L << 4) // Graphics/Video M
#define SOLID_DRAW 	(1L << 5) // Toggles between dot/line drawing mode
#define SHOW_NET    (1L << 6) // Net traffic graphs
#define TRANSPARENT (1L << 7) // Pseudo tranparency
#define DRAGBAR     (1L << 8) // Dragbar

static ULONG cpu_col = CPU_COL;
static ULONG pub_col = PUB_COL;
static ULONG virt_col = VIRT_COL;
static ULONG gfx_col = GFX_COL;
static ULONG grid_col = GRID_COL;
static ULONG bg_col = BG_COL;
static ULONG dl_col = DL_COL;
static ULONG ul_col = UL_COL;

// This set of flags controls which graphs are drawn
static ULONG show_bits = SHOW_CPU | SHOW_GRID | SHOW_PMEM | SHOW_VMEM | SHOW_GMEM | SOLID_DRAW | DRAGBAR;

struct TimerIFace *ITimer;

// Graph data
static UBYTE *cpu;
static UBYTE *virt_mem;
static UBYTE *pub_mem;
static UBYTE *gfx_mem;
static UBYTE *upload;
static UBYTE *download;

// Signals for inter-process synchronization
static BYTE idle_sig = -1;
static BYTE main_sig = -1;

// Graph window
static struct Window *window = NULL;

// Main task's address
static struct Task *main_task;

// Corresponds to seconds ran
static ULONG iter = 0;

// "Volatile" to avoid clever optimizer walking over me. Program runs as long as flag is set.
static volatile BOOL running = TRUE;

// Simple mode switches to non-busy looping option when measuring the CPU usage.
static volatile BOOL simple_mode = FALSE;

// How many times idle task was ran during 1 second. Run count 0 means 100% cpu usage, 100 means 0 % CPU usage
static ULONG run_count = 0;

// Used by idle task for 1/100 second pauses when running in non-busy looping mode
static struct TimeRequest *pause_req = NULL;

// For keeping book of time
static struct TimeVal idle_start, idle_finish, idle_time;

typedef struct {
	struct BitMap *bm;
	struct MsgPort *timer_port, *user_port;
	struct Task *idle_task;
	struct TimeRequest *timer_req;
	BYTE timer_device;
	struct TimeVal tv;
} Context;

static int x_pos = 0;
static int y_pos = 0;

static BOOL idler_trouble = FALSE;

// network.c
void init_netstats(void);
BOOL update_netstats(UBYTE *, UBYTE *, float *, float *, float *, float *);

// Idle task gives up CPU
static void my_switch(void)
{
	GetSysTime(&idle_finish);

	SubTime(&idle_finish, &idle_start);

	idle_time.Seconds += idle_finish.Seconds;
	idle_time.Microseconds += idle_finish.Microseconds;
}

// Idle task gets CPU
static void my_launch(void)
{
	GetSysTime(&idle_start);
}

static void idler(void)
{
	struct MsgPort *idle_port = CreateMsgPort();
	
	if ( ! idle_port ) {
		idler_trouble = TRUE;
		goto die;
	}

	idle_sig = AllocSignal(-1);

	if (idle_sig == -1) {
		idler_trouble = TRUE;
		goto die;
	}

	struct Task *me = FindTask(NULL);

	// Signal main task that we are ready
	Signal( main_task, 1L << main_sig );

	// Wait for main task to obtain ITimer so that timer.device functions can be used
	Wait( 1L << idle_sig );

	if (!ITimer) {
		idler_trouble = TRUE;
		goto die;
	}

	pause_req->Request.io_Message.mn_ReplyPort = idle_port;

	Forbid();
	me->tc_Switch = my_switch;
	me->tc_Launch = my_launch;
	me->tc_Flags |= TF_SWITCH | TF_LAUNCH;
	Permit();

	/* Use minimum priority */
	SetTaskPri( me, -127 );

	while ( running ) {
		if ( simple_mode ) {
			struct TimeVal tv;

			run_count++;

			GetSysTime( &tv );

 			// When in "Simple" mode, pause for 1/100th of second
			tv.Microseconds += 10000; // 100 Hz

			pause_req->Request.io_Command = TR_ADDREQUEST;
			pause_req->Time.Seconds = tv.Seconds;
			pause_req->Time.Microseconds = tv.Microseconds;

			DoIO( (struct IORequest*) pause_req );
		}
	}

	Forbid();
	me->tc_Switch = NULL;
	me->tc_Launch = NULL;
	me->tc_Flags ^= TF_SWITCH | TF_LAUNCH;
	Permit();

	if (idle_sig != -1 ) {
		FreeSignal( idle_sig );
	}

die:

	if (idle_port) {
		DeleteMsgPort(idle_port);
	}

	// Tell the main task that we can leave now (error flag may be set!)
	Signal(main_task, 1L << main_sig);

	// Waiting for termination
	Wait( 0L );
}

/* Plots a graph to bitmap
- ptr is a bitmap pointer
- lpr means "longs per row", ie byter per row / 4
- array is pointer to graph data
*/
static void plot(ULONG* const ptr, const WORD lpr, const UBYTE* const array, const ULONG color)
{
	WORD x, y;
	for ( x=0; x < XSIZE; x++ )
	{
		UBYTE cur_y = array[ (iter+1 + x) % XSIZE ];

		// Plot the current dot
		*(ptr + x + (YSIZE - 1 - cur_y) * lpr ) = color;

		// Make the plotted line solid
		if ( show_bits & SOLID_DRAW )
		{
		    UBYTE prev_y = array[ (iter + x) % XSIZE ];
			WORD diff = cur_y - prev_y;

			if ( x > 0 && diff != 0 )
			{
				if ( diff < 0 )
				{
					UBYTE temp = cur_y;
					cur_y = prev_y;
					prev_y = temp;
				}

				ULONG *_ptr = ptr + x + (YSIZE-1-prev_y) * lpr;

				for ( y = prev_y; y <= cur_y; y++ )
				{
					*_ptr = color;
					_ptr -= lpr;
				}
			}
		}
	}
}

/* Special function to plot 2-sided network graph */
static void plot_net(ULONG* const ptr, const WORD lpr)
{
	WORD x, y;
	for ( x = 0; x < XSIZE; x++ )
	{
		// Upload, above the center line
		WORD start_y = - upload[ (iter+1 + x) % XSIZE ] / 2;
		ULONG *_ptr = ptr + x + ( YSIZE/2 + start_y) * lpr;

		for ( y = start_y; y < 0; y++ )
		{
			*_ptr = ul_col;
			_ptr += lpr;
		}

		// Download, below the center line
		WORD end_y = download[ (iter+1 + x) % XSIZE] / 2;
		_ptr = ptr + x + (YSIZE/2+1) * lpr;

		for ( y = 1 ; y <= end_y; y++ )
		{
			*_ptr = dl_col;
			_ptr += lpr;
		}
	}
}

static void refresh_window(Context *ctx)
{
	if (show_bits & TRANSPARENT)
	{
		// TODO
	}

	struct RenderInfo ri;
	ULONG lock = p96LockBitMap( ctx->bm, (UBYTE *)&ri, sizeof(struct RenderInfo) );

	if ( lock )
	{
		ULONG* ptr = (ULONG*) ri.Memory;
		LONG lpr = ri.BytesPerRow / 4;

		WORD x, y;

		// Clear background to black
		//if ( ! (show_bits & TRANSPARENT) )
		{
			//memset( ri.Memory, 0, (window->Height - (window->BorderBottom + window->BorderTop)) * ri.BytesPerRow );

			ptr = (ULONG*) ri.Memory;
			for (y = 0; y < window->Height-(window->BorderBottom + window->BorderTop); y++)
			{
				for (x = 0; x < XSIZE; x++)
				{
					ptr[x] = bg_col;
				}

				ptr += lpr;
			}
		}

		ptr = (ULONG*) ri.Memory;

		if ( show_bits & SHOW_GRID )
		{
			// Horizontal lines
			for ( y = 0; y < YSIZE; y += 10 )
			{
				for ( x = 0; x < XSIZE; x++ )
				{
					ptr[x] = grid_col;
				}
				ptr += 10 * lpr;
			}

			ptr = (ULONG*) ri.Memory;

			// Vertical lines
			for ( x = 0; x < XSIZE; x += 60 )
			{
				ULONG* _ptr = ptr + x;

	            for ( y = 0; y < YSIZE; y++ )
		        {
					*_ptr = grid_col;
					_ptr += lpr;
				}
			}
		}

		if ( show_bits & SHOW_PMEM )
		{
			plot( ptr, lpr, pub_mem, pub_col );
		}

		if ( show_bits & SHOW_VMEM )
		{
			plot( ptr, lpr, virt_mem, virt_col );
		}

		if ( show_bits & SHOW_GMEM )
		{
			plot( ptr, lpr, gfx_mem, gfx_col );
		}

		if ( show_bits & SHOW_CPU )
		{
			plot( ptr, lpr, cpu, cpu_col );
		}

		if ( show_bits & SHOW_NET )
		{
			ptr += lpr * YSIZE;

			if ( show_bits & SHOW_GRID )
			{
				// Horizontal lines
				for ( y = 0; y < YSIZE; y += 10 )
				{
					for ( x = 0; x < XSIZE; x++ )
					{
						ptr[x] = grid_col;
					}
					ptr += 10 * lpr;
				}

				ptr = (ULONG*) ri.Memory + lpr * YSIZE;

				// Vertical lines
				for ( x = 0; x < XSIZE; x += 60 )
				{
					ULONG* _ptr = ptr + x;

		            for ( y = 0; y < YSIZE; y++ )
			        {
						*_ptr = grid_col;
						_ptr += lpr;
					}
				}
			}

			ptr = (ULONG*) ri.Memory + lpr * YSIZE;

			//plot( ptr, lpr, download, DL_COL );
			//plot( ptr, lpr, upload, UL_COL );

			plot_net( ptr, lpr );
		}

		p96UnlockBitMap( ctx->bm, lock );
	}

	BltBitMapRastPort(ctx->bm, 0, 0, window->RPort, window->BorderLeft, window->BorderTop,
		window->Width - (window->BorderRight + window->BorderLeft),
		window->Height - (window->BorderBottom + window->BorderTop),
		0xC0 );
}

ULONG parse_hex(STRPTR str)
{
	return strtol(str, NULL, 16);
}

void read_config(STRPTR file_name)
{
	if (file_name) {

		struct DiskObject * disk_object = (struct DiskObject *)GetDiskObject(file_name);
		if (disk_object)
		{
			// Reset
			show_bits = 0;

			{
			STRPTR cpu = FindToolType(disk_object->do_ToolTypes, "cpu");
			if (cpu)
			{
				//if (MatchToolValue(cpu, "True"))
				{
				//	  printf("cpu\n");
					show_bits |= SHOW_CPU;
				}
			}}

			{
			STRPTR grid = FindToolType(disk_object->do_ToolTypes, "grid");
			if (grid)
			{
				//if (MatchToolValue(grid, "True"))
				{
				//	  printf("grid\n");
					show_bits |= SHOW_GRID;
				}
			}}


			{
			STRPTR pmem = FindToolType(disk_object->do_ToolTypes, "pmem");
			if (pmem)
			{
				//if (MatchToolValue(pmem, "True"))
				{
				//	  printf("pmem\n");
					show_bits |= SHOW_PMEM;
				}
			}}

			{
			STRPTR vmem = FindToolType(disk_object->do_ToolTypes, "vmem");
			if (vmem)
			{
				//if (MatchToolValue(vmem, "True"))
				{
				//	  printf("vmem\n");
					show_bits |= SHOW_VMEM;
				}
			}}

			{
			STRPTR gmem = FindToolType(disk_object->do_ToolTypes, "gmem");
			if (gmem)
			{
				//if (MatchToolValue(gmem, "True"))
				{
				//	  printf("gmem\n");
					show_bits |= SHOW_GMEM;
				}
			}}

			{
			STRPTR solid = FindToolType(disk_object->do_ToolTypes, "solid");
			if (solid)
			{
				//if (MatchToolValue(solid, "True"))
				{
				//	  printf("soliddraw\n");
					show_bits |= SOLID_DRAW;
				}
			}}

			{
			STRPTR transparent = FindToolType(disk_object->do_ToolTypes, "transparent");
			if (transparent)
			{
				//if (MatchToolValue(transparent, "True"))
				{
				//	  printf("transparent\n");
					show_bits |= TRANSPARENT;
				}
			}}

			{
			STRPTR net = FindToolType(disk_object->do_ToolTypes, "net");
			if (net)
			{
				//if (MatchToolValue(net, "True"))
				{
				//	  printf("net\n");
					show_bits |= SHOW_NET;
				}
			}}

			{
			STRPTR dragbar = FindToolType(disk_object->do_ToolTypes, "dragbar");
			if (dragbar)
			{
				//if (MatchToolValue(dragbar, "True"))
				{
				//	  printf("dragbar\n");
					show_bits |= DRAGBAR;
				}
			}}

			{
			STRPTR x = FindToolType(disk_object->do_ToolTypes, "xpos");
			if (x)
			{
				const int temp = atoi(x);
				if (temp >= 0)
				{
					x_pos = temp;
				}
			}}

			{
			STRPTR y = FindToolType(disk_object->do_ToolTypes, "ypos");
			if (y)
			{
				const int temp = atoi(y);
				if (temp >= 0)
				{
					y_pos = temp;
				}
			}}

			{
			STRPTR simple = FindToolType(disk_object->do_ToolTypes, "simple");
			if (simple)
			{
				simple_mode = TRUE;
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "cpucol");
			if (col)
			{
				cpu_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "bgcol");
			if (col)
			{
				bg_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "gmemcol");
			if (col)
			{
				gfx_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "pmemcol");
			if (col)
			{
				pub_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "vmemcol");
			if (col)
			{
				virt_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "gridcol");
			if (col)
			{
				grid_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "ulcol");
			if (col)
			{
				ul_col = parse_hex(col);
			}}

			{
			STRPTR col = FindToolType(disk_object->do_ToolTypes, "dlcol");
			if (col)
			{
				dl_col = parse_hex(col);
			}}

			FreeDiskObject(disk_object);
		}
	}
}

static void handle_args(int argc, char ** argv)
{
	if (! argc) {
		struct WBStartup * wb_startup = (struct WBStartup *) argv;
		struct WBArg * wb_arg = wb_startup->sm_ArgList;

		if (wb_arg /*&& wb_arg->wa_Lock*/) {
			//BPTR old_dir = GetCurrentDir(/*wb_arg->wa_Lock*/);

			read_config(wb_arg->wa_Name);

			//CurrentDir(old_dir);
		}
	} else {
		read_config(argv[0]);
	}
}

BOOL allocate_resources(Context *ctx)
{
	BOOL result = FALSE;

	main_sig = AllocSignal( -1 );

	if ( main_sig == -1 ) {
		printf("Couldn't allocate signal\n");
		goto clean;
	}

	cpu = AllocVec( XSIZE * sizeof(UBYTE), MEMF_ANY|MEMF_CLEAR );

	pub_mem = AllocVec( XSIZE * sizeof(UBYTE), MEMF_ANY|MEMF_CLEAR );
	virt_mem = AllocVec( XSIZE * sizeof(UBYTE), MEMF_ANY|MEMF_CLEAR );
	gfx_mem = AllocVec( XSIZE * sizeof(UBYTE), MEMF_ANY|MEMF_CLEAR );

	upload = AllocVec( XSIZE * sizeof(UBYTE), MEMF_ANY|MEMF_CLEAR );
	download = AllocVec( XSIZE * sizeof(UBYTE), MEMF_ANY|MEMF_CLEAR );

	if ( ! ( cpu && pub_mem && virt_mem && gfx_mem && upload && download ) ) {
		printf("Out of memory\n");
		goto clean;
	}

	// Draw buffer
	ctx->bm = p96AllocBitMap( XSIZE, 2*YSIZE, 32, BMF_CLEAR/*|BMF_USERPRIVATE*/, NULL, RGBFB_A8R8G8B8 );
	if ( ! ctx->bm ) {
		printf("Couln't allocate bitmap\n");
		goto clean;
	}

	ctx->user_port = CreateMsgPort();
	if ( ! ctx->user_port ) {
		printf("Couldn't create user port\n");
		goto clean;
	}

	ctx->idle_task = CreateTask( "Uuno", 0, idler, 4096, NULL );

	if ( !ctx->idle_task ) {
		printf("Couldn't create idler task\n");
		goto clean;
	}

	ctx->timer_port = CreateMsgPort();

	if ( !ctx->timer_port ) {
		printf("Couldn't create timer port\n");
		goto clean;
	}

	ctx->timer_req = (struct TimeRequest *) CreateIORequest(ctx->timer_port, sizeof(struct TimeRequest) );

	if ( !ctx->timer_req ) {
		printf("Couldn't create IO request\n");
		goto clean;
	}

	if ( ( ctx->timer_device = OpenDevice( TIMERNAME, UNIT_WAITUNTIL, (struct IORequest *) ctx->timer_req, 0 ) ) ) {
    	printf("Couldn't open timer.device\n");
		goto clean;
	}

	ITimer = (struct TimerIFace *) GetInterface( (struct Library *) ctx->timer_req->Request.io_Device, "main", 1, NULL );
	
	if ( !ITimer ) {
		printf("Couldn't get Timer interface\n");
		goto clean;
	}

	pause_req = AllocVec( sizeof(struct TimeRequest), MEMF_ANY | MEMF_CLEAR );
	
	if ( ! pause_req ) {
		printf("Couldn't allocate TimeRequest\n");
		goto clean;
	}

	CopyMem( ctx->timer_req, pause_req, sizeof(struct TimeRequest) );

	window = OpenWindowTags( NULL,
		WA_Left, x_pos,
		WA_Top, y_pos,
		WA_InnerWidth, XSIZE,
		WA_InnerHeight, (show_bits & SHOW_NET) ? 2 * YSIZE : YSIZE,
		WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_CHANGEWINDOW,
		WA_CloseGadget, (show_bits & DRAGBAR) ? TRUE : FALSE,
		WA_DragBar, (show_bits & DRAGBAR) ? TRUE : FALSE,
		WA_DepthGadget, (show_bits & DRAGBAR) ? TRUE : FALSE,
		WA_UserPort, ctx->user_port,
		TAG_DONE );

	if ( ! window ) {
		printf("Couldn't open window\n");
		goto clean;
	}

	result = TRUE;

clean:

	return result;
}

static void handle_window_events(Context *ctx)
{
	struct IntuiMessage *msg;
	while ( (msg = (struct IntuiMessage*) GetMsg( window->UserPort ) ) )
	{
		const ULONG Class = msg->Class;
		const UWORD Code = msg->Code;

		const ULONG seconds = msg->Seconds;
		const ULONG micros = msg->Micros;

		ReplyMsg( (struct Message*) msg );

		if ( Class == IDCMP_CLOSEWINDOW )
		{
		 	running = FALSE;
		}
		else if ( Class == IDCMP_VANILLAKEY )
		{
			BOOL update = FALSE;

			switch ( Code )
			{
				case 'c':
					show_bits ^= SHOW_CPU;
					update = TRUE;
					break;

				case 'p':
					show_bits ^= SHOW_PMEM;
					update = TRUE;
					break;

				case 'v':
					show_bits ^= SHOW_VMEM;
					update = TRUE;
					break;

				case 'x':
					show_bits ^= SHOW_GMEM;
					update = TRUE;
					break;

				case 'g':
					show_bits ^= SHOW_GRID;
					update = TRUE;
					break;

				case 's':
					show_bits ^= SOLID_DRAW;
					update = TRUE;
					break;

				case 't':
					show_bits ^= TRANSPARENT;
					update = TRUE;
					if ( show_bits & TRANSPARENT)
					{
						//TODO
					}
					break;

				case 'm':
					simple_mode ^= TRUE;
					break;

				case 'n':
					show_bits ^= SHOW_NET;
					SizeWindow(window, 0, (show_bits & SHOW_NET) ? YSIZE : -YSIZE);
					update = TRUE;
					break;

				case 'd':
					show_bits ^= DRAGBAR;

					// Store old coordinates
					const WORD x = window->LeftEdge;
					const WORD y = window->TopEdge;

					// Window is using WA_UserPort, so CloseWindow() will do CloseWindowSafely()
					CloseWindow( window );

					window = OpenWindowTags( NULL,
						WA_Left, x,
						WA_Top, y,
						WA_InnerWidth, XSIZE,
						WA_InnerHeight, (show_bits & SHOW_NET) ? 2 * YSIZE : YSIZE,
						WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_CHANGEWINDOW,
						WA_CloseGadget, (show_bits & DRAGBAR) ? TRUE : FALSE,
						WA_DragBar, (show_bits & DRAGBAR) ? TRUE : FALSE,
						WA_DepthGadget, (show_bits & DRAGBAR) ? TRUE : FALSE,
						WA_UserPort, ctx->user_port,
						TAG_DONE );

					if ( !window )
					{
						printf("Panic - can't reopen window!\n");
						running = FALSE;
					}
					else
					{
						ActivateWindow( window );
						update = TRUE;
					}
					break;

				case 'q':
					running = FALSE;
					break;

				default:
					break;
			};

            if ( update )
            {
				refresh_window(ctx);
            }

		}
		else if ( Class == IDCMP_CHANGEWINDOW )
		{
			// Work-around for queued window move events, causes blinking in solid window move mode!
			static ULONG last_time = 0;

			if (last_time + 500000 > seconds * 1000000 + micros )
			{

			}
			else
			{
				last_time = seconds * 1000000 + micros;
			}
		}
	}
}

static void handle_timer_events(Context *ctx)
{
	// Window string is a bit shorter, window is currently only about 300 pixels wide anyway
	/*static*/ char *w_format 		= "CPU: %3d%% V: %3d%% P: %3d%% G: %3d%%";
	/*static*/ char *s_format 		= "CPU: %3d%% Virtual: %3d%% Public: %3d%% Graphics: %3d%% Download: %4.1fKB/s Upload: %4.1fKB/s Mode: %c";
	/*static*/ char w_title_string[] = "CPU: 100%% V: 100%% P: 100%% G: 100%%";
	/*static*/ char s_title_string[] = "CPU: 100%% Virtual: 100%% Public: 100%% Graphics: 100%% Download: 9999.9KB/s Upload: 9999.9KB/s Mode: S";

	GetMsg( ctx->timer_port );

	//tv.tv_micro += 1000000/FREQ;
	ctx->tv.Seconds += 1;

	ctx->timer_req->Request.io_Command = TR_ADDREQUEST;
	ctx->timer_req->Time.Seconds = ctx->tv.Seconds;
	ctx->timer_req->Time.Microseconds = ctx->tv.Microseconds;

	SendIO( (struct IORequest*) ctx->timer_req );

	// Timer signal, update visuals once per second
	//if ( (++count % FREQ) == 0)
	{
		UBYTE value = 100;

		if ( simple_mode )
		{
			value -= run_count;
			//printf("value %d %ld\n", value, run_count);
		}
		else
		{
			value -= 100 * (idle_time.Seconds * 1000000 + idle_time.Microseconds) / (float) 1000000;
		}

		// Reset idle time
		idle_time.Seconds = 0;
		idle_time.Microseconds = 0;
		run_count = 0;

		if ( value > 100 ) value = 100;
		cpu[ ++iter % XSIZE ] = value;

		// Query free memory
        value = 100 * (float) AvailMem( MEMF_PUBLIC ) / AvailMem(MEMF_PUBLIC|MEMF_TOTAL);
        if ( value > 100 ) value = 100;
		pub_mem[ iter % XSIZE ] = value;

		value = 100 * (float) AvailMem( MEMF_VIRTUAL ) / AvailMem(MEMF_VIRTUAL|MEMF_TOTAL);
        if ( value > 100 ) value = 100;
		virt_mem[ iter % XSIZE ] = value;

		ULONG free, total;
		p96GetBoardDataTags( 0, P96BD_FreeMemory, &free, P96BD_TotalMemory, &total, TAG_DONE );

		value = 100 * (float) free / total;
        if ( value > 100 ) value = 100;
		gfx_mem[ iter % XSIZE ] = value;

		//
		// Network traffic
		//

		// Down/Up load speed in Kilobytes
		float dl_speed = 0, ul_speed = 0;

		float dl_mult = 1.0f, ul_mult = 1.0f;
		UBYTE dl_p, ul_p;

		if ( update_netstats( &dl_p, &ul_p, &dl_mult, &ul_mult, &dl_speed, &ul_speed ) )
		{
			UWORD i;
			for ( i = 0; i < XSIZE; i++ )
			{
				download[ i ] *= dl_mult;
				upload[ i ] *= ul_mult;
			}
		}

		download[ iter % XSIZE ] = dl_p;
		upload[ iter % XSIZE ] = ul_p;

  		// Update window (if dragbar) & screen titles too
		sprintf( w_title_string, w_format, cpu[ iter % XSIZE ], virt_mem[ iter % XSIZE ], pub_mem[ iter % XSIZE ], gfx_mem[ iter % XSIZE ] );
		sprintf( s_title_string, s_format, cpu[ iter % XSIZE ], virt_mem[ iter % XSIZE ], pub_mem[ iter % XSIZE ], gfx_mem[ iter % XSIZE ], dl_speed, ul_speed, simple_mode ? 'S' : 'B' );
		SetWindowTitles( window, (show_bits & DRAGBAR) ? w_title_string : NULL, s_title_string );

		refresh_window(ctx);
   	}
}

static void start_timer(Context *ctx)
{
	GetSysTime( &ctx->tv );

	ctx->tv.Seconds += 1;

	ctx->timer_req->Request.io_Command = TR_ADDREQUEST;
	ctx->timer_req->Time.Seconds = ctx->tv.Seconds;
	ctx->timer_req->Time.Microseconds = ctx->tv.Microseconds;

	SendIO( (struct IORequest*) ctx->timer_req );
}

static void stop_timer(Context *ctx)
{
	if ( ! CheckIO( (struct IORequest*) ctx->timer_req ) ) {
		AbortIO( (struct IORequest*) ctx->timer_req );
		WaitIO( (struct IORequest*) ctx->timer_req );
	}
}

static void free_resources(Context *ctx)
{
	// if idler task had problems, don't wait for it
	if ( ctx->idle_task && !idler_trouble ) {
	    // Give it some more cpu
		SetTaskPri( ctx->idle_task, 0 );

	 	// Wait idler task to finish possible timer actions before closing timing services
		Wait( 1L << main_sig | SIGBREAKF_CTRL_C );
	}

	if (ITimer) {
		DropInterface( (struct Interface*) ITimer );
	}

	if ( ctx->timer_device == 0 && ctx->timer_req ) {
		CloseDevice( (struct IORequest*) ctx->timer_req );
	}

	if ( ctx->timer_req ) {
		DeleteIORequest( (struct IORequest*) ctx->timer_req );
	}

	if (pause_req) {
		FreeVec( pause_req );
	}

	if ( ctx->timer_port ) {
		DeleteMsgPort( ctx->timer_port );
	}

	if ( ctx->idle_task ) {
		DeleteTask( ctx->idle_task );
	}

	if (main_sig != -1) {
		FreeSignal( main_sig );
	}

	if (window) {
		CloseWindow( window );
	}

	if ( ctx->user_port ) {
		DeleteMsgPort( ctx->user_port );
	}

	if (ctx->bm) {
		p96FreeBitMap( ctx->bm );
	}

	if (cpu) FreeVec( cpu );

	if (gfx_mem) FreeVec( gfx_mem );
	if (pub_mem) FreeVec( pub_mem );
	if (virt_mem) FreeVec( virt_mem );

	if (upload) FreeVec( upload );
	if (download) FreeVec( download );
}

static void init_context(Context *ctx)
{
	memset(ctx, 0, sizeof(Context));

	ctx->timer_device = -1;
}

static void main_loop(Context *ctx)
{
	// Reset idle time
	//idle_time.Seconds = 0;
	//idle_time.Microseconds = 0;

	while ( running ) {
		ULONG sigs = Wait( SIGBREAKF_CTRL_C | 1L << ctx->timer_port->mp_SigBit | 1L << window->UserPort->mp_SigBit);

		if ( sigs & (1L << ctx->timer_port->mp_SigBit) ) {
			handle_timer_events(ctx);
		}

		if ( sigs & 1L << window->UserPort->mp_SigBit ) {
			handle_window_events(ctx);
		}

		if ( sigs & SIGBREAKF_CTRL_C ) {
			running = FALSE;
		}
	}
}

BOOL sync_to_idler_task(Context *ctx)
{
	BOOL result = FALSE;

	Wait( 1L << main_sig );

	if ( idler_trouble ) {
		goto clean;
	}

	// Send message, "We've got ITimer now", back
	Signal( ctx->idle_task, 1L << idle_sig );

	result = TRUE;

clean:
	return result;
}

int main(int argc, char ** argv)
{
	Context ctx;
	init_context(&ctx);

	handle_args(argc, argv);

	if (allocate_resources(&ctx) == FALSE) {
		goto clean;
	}

	main_task = FindTask( NULL );

	if (sync_to_idler_task(&ctx) == FALSE) {
		goto clean;
	}

	refresh_window( &ctx );
	init_netstats();

	start_timer(&ctx);
	main_loop(&ctx);

	stop_timer(&ctx);

clean:

	free_resources(&ctx);

	return 0;
}
