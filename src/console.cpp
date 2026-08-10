/*
 * console.cpp -- coordinated console output for DIRTYBIRD C Miner
 *
 * One mutex serializes every write to the console so timestamped event lines
 * (log_line) never corrupt the in-place status line drawn by the reporter
 * thread. Lives in dluna_runtime so both the miner and the PGO trainer (which
 * compile miner.cpp without main.cpp) resolve these symbols.
 */

#include "dluna.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>   /* getenv, strtol */
#include <climits>   /* INT_MAX */
#include <ctime>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <io.h>      /* _isatty, _fileno */
#else
#include <unistd.h>     /* isatty, fileno, STDOUT_FILENO */
#include <sys/ioctl.h>  /* ioctl, TIOCGWINSZ, struct winsize */
#endif

std::mutex g_console_mtx;       /* serializes all console writes */
bool       g_verbose = false;   /* -V / DLUNA_VERBOSE: restore event spam */

/* When stdout is a real console we rewrite one line in place with \r + \033[K.
 * When it is redirected to a file/pipe, \r/\033[K are inert garbage, so we
 * fall back to plain newline-terminated periodic lines. */
static bool g_tty   = true;     /* stdout is an interactive console */
static bool g_vt_ok = true;     /* VT (ANSI \033[K) sequences usable */

bool dluna_is_tty(void) { return g_tty && g_vt_ok; }

/* Clears from the cursor to end of line. On a VT-capable console it is the ANSI
 * erase-to-EOL; otherwise empty (the status line space-pads instead). */
const char *dluna_clr_eol(void) { return dluna_is_tty() ? "\033[K" : ""; }

void dluna_console_init(void)
{
#ifdef _WIN32
	g_tty = _isatty(_fileno(stdout)) != 0;
	if (g_tty) {
		SetConsoleOutputCP(CP_UTF8);
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD mode = 0;
		if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
			g_vt_ok = SetConsoleMode(
				h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
		} else {
			g_vt_ok = false;
		}
	} else {
		g_vt_ok = false;
	}
#else
	g_tty   = isatty(fileno(stdout)) != 0;
	g_vt_ok = g_tty; /* POSIX terminals understand \033[K */
#endif
}

/* --- terminal width --- */

/* Assumed width when stdout IS a console but its size cannot be read (some
 * Termux/HiveOS/detached-screen setups). Guessing the classic 80 is what keeps
 * the status line on one row: the old "unknown" answer of 0 meant an unbounded
 * budget, so the 115-column full layout went out to a terminal of any size and
 * wrapped. 80 is never worse -- a wider console just gets a narrower layout,
 * while a narrower one is the case that actually breaks. */
static const int DEFAULT_COLS = 80;

/* Visible console width in columns. 0 means "not a console" (redirected to a
 * file or pipe) -- never "console of unknown size", which reports DEFAULT_COLS
 * instead. Queried fresh on every reporter tick (one cheap syscall per second on
 * a cold path), which is how a font-size change or a window resize is picked up
 * without a SIGWINCH handler. */
int dluna_term_cols(void)
{
	/* Explicit override wins: TIOCGWINSZ reports 0 under some HiveOS/MMPOS
	 * and detached-screen setups, and tests want a deterministic width. */
	if (const char *env = std::getenv("DIRTYBIRD_C_COLS")) {
		long v = std::strtol(env, nullptr, 10);
		if (v > 0 && v < 10000)
			return (int)v;
	}
	if (!dluna_is_tty())
		return 0;
#ifdef _WIN32
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &csbi))
		return DEFAULT_COLS;
	/* conhost wraps at the screen BUFFER width but only the window is on
	 * screen; the smaller of the two keeps the line both unwrapped and
	 * fully visible. On Windows Terminal the two are equal anyway. */
	int buf = (int)csbi.dwSize.X;
	int win = (int)(csbi.srWindow.Right - csbi.srWindow.Left) + 1;
	int cols = (buf > 0 && buf < win) ? buf : win;
	return cols > 0 ? cols : DEFAULT_COLS;
#else
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	return DEFAULT_COLS;
#endif
}

/* --- status line rendering --- */

/* The status line is rewritten in place with a bare \r, so it MUST fit on one
 * terminal row: \r rewinds to the start of the row the cursor is on, and a line
 * that auto-wraps leaves the cursor on its LAST row -- every repaint then starts
 * a row lower and the line stacks down the screen instead of updating in place.
 * (That is the Termux bug: 115 visible columns on a 56-column phone terminal.)
 *
 * So we render the widest of four layouts whose VISIBLE width fits the terminal.
 * Visible width has to be tracked separately from byte length because ~75 of the
 * bytes are ANSI escapes that occupy no column -- and for the same reason the
 * line can never be truncated by byte offset: that both corrupts colour state
 * and does not shorten the line predictably. */

namespace {

/* Per-field colours, identical across every layout so it still reads as the
 * same miner when it shrinks. */
#define C_LABEL  "\033[93m"   /* [DIRTYBIRD-C] / [DB]  bright yellow */
#define C_RATE   "\033[92m"   /* instantaneous KH/s  bright green  */
#define C_TEXT   "\033[97m"   /* separators          bright white  */
#define C_AVG    "\033[32m"   /* average KH/s        green         */
#define C_HEIGHT "\033[34m"   /* Height              blue          */
#define C_MINI   "\033[36m"   /* Miniblocks          cyan          */
#define C_BLOCK  "\033[32m"   /* Blocks              green         */
#define C_DIFF   "\033[35m"   /* Diff                magenta       */
#define C_TIME   "\033[37m"   /* uptime              white         */
#define C_REJ_OK "\033[37m"   /* REJ == 0            white         */
#define C_REJ_HI "\033[91m"   /* REJ  > 0            bright red    */
#define C_RESET  "\033[0m"

enum Tier { TIER_FULL = 0, TIER_MEDIUM, TIER_NARROW, TIER_COMPACT, TIER_MINIMAL,
            TIER_COUNT };

/* Append-only writer that counts columns, not bytes. */
struct LineBuf {
	char  *b;
	size_t cap;
	size_t n   = 0;   /* bytes written */
	int    vis = 0;   /* columns the cursor advances */
	bool   ovf = false;
};

/* Bytes that occupy no column: ANSI escapes and the leading \r. */
void lb_esc(LineBuf &lb, const char *seq)
{
	if (lb.ovf)
		return;
	size_t len = std::strlen(seq);
	if (lb.n + len + 1 > lb.cap) { lb.ovf = true; return; }
	std::memcpy(lb.b + lb.n, seq, len);
	lb.n += len;
	lb.b[lb.n] = '\0';
}

/* Printable text. Every layout is pure ASCII, so bytes == columns here. */
#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void lb_txt(LineBuf &lb, const char *fmt, ...)
{
	if (lb.ovf)
		return;
	va_list ap;
	va_start(ap, fmt);
	int len = vsnprintf(lb.b + lb.n, lb.cap - lb.n, fmt, ap);
	va_end(ap);
	if (len < 0 || (size_t)len >= lb.cap - lb.n) {
		lb.ovf = true;
		lb.b[lb.n] = '\0';
		return;
	}
	lb.n   += (size_t)len;
	lb.vis += len;
}

void render_tier(LineBuf &lb, const DlunaStatus &s, int tier)
{
	const char *rej = (s.rejected > 0) ? C_REJ_HI : C_REJ_OK;

	lb_esc(lb, "\r");
	switch (tier) {
	case TIER_FULL:  /* the canonical full line (rebranded label) */
		lb_esc(lb, C_LABEL);  lb_txt(lb, "[DIRTYBIRD-C] ");
		lb_esc(lb, C_RATE);   lb_txt(lb, "%.2f KH/s", s.rate);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " (");
		lb_esc(lb, C_AVG);    lb_txt(lb, "%.2f KH/s avg", s.avg);
		lb_esc(lb, C_TEXT);   lb_txt(lb, ") | ");
		lb_esc(lb, C_HEIGHT); lb_txt(lb, "Height:%lld", s.height);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_MINI);   lb_txt(lb, "Miniblocks:%lld", s.accepted);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_BLOCK);  lb_txt(lb, "Blocks:%lld", s.blocks);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, rej);      lb_txt(lb, "REJ:%lld", s.rejected);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_DIFF);   lb_txt(lb, "Diff:%s", s.diff);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_TIME);   lb_txt(lb, "%02d:%02d:%02d", s.hh, s.mm, s.ss);
		break;
	case TIER_MEDIUM:  /* abbreviated labels, every field kept */
		lb_esc(lb, C_LABEL);  lb_txt(lb, "[DIRTYBIRD-C] ");
		lb_esc(lb, C_RATE);   lb_txt(lb, "%.2f KH/s", s.rate);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " (");
		lb_esc(lb, C_AVG);    lb_txt(lb, "%.2f avg", s.avg);
		lb_esc(lb, C_TEXT);   lb_txt(lb, ") | ");
		lb_esc(lb, C_HEIGHT); lb_txt(lb, "H:%lld", s.height);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_MINI);   lb_txt(lb, "MB:%lld", s.accepted);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_BLOCK);  lb_txt(lb, "B:%lld", s.blocks);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, rej);      lb_txt(lb, "R:%lld", s.rejected);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_DIFF);   lb_txt(lb, "D:%s", s.diff);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_TIME);   lb_txt(lb, "%02d:%02d:%02d", s.hh, s.mm, s.ss);
		break;
	case TIER_NARROW:  /* fits the classic 80-column terminal: drops the average */
		lb_esc(lb, C_LABEL);  lb_txt(lb, "[DIRTYBIRD-C] ");
		lb_esc(lb, C_RATE);   lb_txt(lb, "%.2f KH/s", s.rate);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_HEIGHT); lb_txt(lb, "H:%lld", s.height);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_MINI);   lb_txt(lb, "MB:%lld", s.accepted);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_BLOCK);  lb_txt(lb, "B:%lld", s.blocks);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, rej);      lb_txt(lb, "R:%lld", s.rejected);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_DIFF);   lb_txt(lb, "D:%s", s.diff);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_TIME);   lb_txt(lb, "%02d:%02d:%02d", s.hh, s.mm, s.ss);
		break;
	case TIER_COMPACT:  /* phone-sized: drops the difficulty too */
		lb_esc(lb, C_LABEL);  lb_txt(lb, "[DB] ");
		lb_esc(lb, C_RATE);   lb_txt(lb, "%.2f KH/s", s.rate);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_HEIGHT); lb_txt(lb, "H:%lld", s.height);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " ");
		lb_esc(lb, C_MINI);   lb_txt(lb, "MB:%lld", s.accepted);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " ");
		lb_esc(lb, C_BLOCK);  lb_txt(lb, "B:%lld", s.blocks);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " ");
		lb_esc(lb, rej);      lb_txt(lb, "R:%lld", s.rejected);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " | ");
		lb_esc(lb, C_TIME);   lb_txt(lb, "%02d:%02d:%02d", s.hh, s.mm, s.ss);
		break;
	default:  /* TIER_MINIMAL -- rate plus the two counters worth watching */
		lb_esc(lb, C_LABEL);  lb_txt(lb, "[DB] ");
		lb_esc(lb, C_RATE);   lb_txt(lb, "%.2f KH/s", s.rate);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " ");
		lb_esc(lb, C_MINI);   lb_txt(lb, "MB:%lld", s.accepted);
		lb_esc(lb, C_TEXT);   lb_txt(lb, " ");
		lb_esc(lb, C_BLOCK);  lb_txt(lb, "B:%lld", s.blocks);
		break;
	}
	lb_esc(lb, C_RESET);
	lb_esc(lb, dluna_clr_eol());
}

/* Last-resort truncation for terminals narrower than even TIER_MINIMAL. Copies
 * every escape whole -- never cutting one in half, which would leave the colour
 * state wedged and print the escape's tail as text -- and counts only printable
 * bytes against the budget. The trailing reset + erase-to-EOL survive for free
 * because they are escapes. Compacts in place; the write index never passes the
 * read index. Returns the new length. */
size_t ansi_clamp(char *buf, size_t n, int budget)
{
	if (budget < 0)
		budget = 0;
	size_t r = 0, w = 0;
	int vis = 0;
	while (r < n) {
		unsigned char c = (unsigned char)buf[r];
		if (c == 0x1b) {                       /* ESC [ params interm final */
			size_t start = r++;
			if (r < n && buf[r] == '[') {
				++r;
				while (r < n && (unsigned char)buf[r] >= 0x30 &&
				                (unsigned char)buf[r] <= 0x3f) ++r;
				while (r < n && (unsigned char)buf[r] >= 0x20 &&
				                (unsigned char)buf[r] <= 0x2f) ++r;
				if (r < n) ++r;
			}
			size_t len = r - start;
			std::memmove(buf + w, buf + start, len);
			w += len;
			continue;
		}
		if (c < 0x20) {                        /* \r -- zero width, keep */
			buf[w++] = buf[r++];
			continue;
		}
		if (vis < budget) { buf[w++] = buf[r++]; ++vis; }
		else              { ++r; }             /* drop printable overflow */
	}
	buf[w] = '\0';
	return w;
}

} /* namespace */

size_t dluna_format_status(char *out, size_t cap, const DlunaStatus &s, int cols)
{
	if (!out || cap == 0)
		return 0;
	out[0] = '\0';

	/* cols - 1, not cols: a line that ends exactly in the last column leaves
	 * the cursor in a terminal-specific pending-wrap state, and some emulators
	 * resolve it by scrolling. One spare column sidesteps the whole question. */
	const int budget = (cols > 0) ? cols - 1 : INT_MAX;

	for (int tier = TIER_FULL; tier < TIER_COUNT; ++tier) {
		LineBuf lb{out, cap};
		render_tier(lb, s, tier);
		if (lb.ovf)
			continue;                  /* buffer too small for this layout */
		if (lb.vis <= budget)
			return lb.n;
	}

	LineBuf lb{out, cap};
	render_tier(lb, s, TIER_MINIMAL);
	return ansi_clamp(out, lb.n, budget);
}

/* Timestamped, mutex-guarded log line: "DD/MM HH:MM:SS.mmm  LEVEL  msg".
 * The leading \r + clear-to-EOL wipes any partial in-place status line so the
 * event starts clean at column 0; its trailing \n drops the cursor to a fresh
 * line, and the next reporter tick redraws the status line below it. */
void log_line(const char *level, const char *fmt, ...)
{
	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()).count() % 1000);

	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	std::lock_guard<std::mutex> lk(g_console_mtx);
	/* localtime() is safe here: every caller holds g_console_mtx, and it
	 * sidesteps the localtime_s signature differences across toolchains. */
	std::tm tm = *std::localtime(&t);
	char ts[32];
	std::strftime(ts, sizeof ts, "%d/%m %H:%M:%S", &tm);
	if (dluna_is_tty())
		printf("\r%s%s.%03d  %-5s %s\n", dluna_clr_eol(), ts, ms, level, msg);
	else
		printf("%s.%03d  %-5s %s\n", ts, ms, level, msg);
	fflush(stdout);
}
