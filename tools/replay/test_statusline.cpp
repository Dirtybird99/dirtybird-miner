/*
 * test_statusline.cpp -- dluna_format_status() must always produce ONE row.
 *
 * The Termux bug this guards against: the status line is repainted with a bare
 * \r, which rewinds to the start of the row the cursor is on. A line wider than
 * the terminal auto-wraps, so the cursor ends on its LAST row and every repaint
 * starts a row lower -- the line stacks down the screen instead of updating in
 * place. 115 visible columns on a 56-column phone terminal cost two rows per
 * second.
 *
 * So the invariant under test is: visible width (ANSI escapes excluded) never
 * exceeds cols - 1, at every terminal width, for every field magnitude -- and
 * the escapes survive intact while that is enforced.
 */

#include "dluna.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		if (!(cond)) {                                               \
			++g_failures;                                        \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
			std::printf(__VA_ARGS__);                            \
			std::printf("\n");                                   \
		}                                                            \
	} while (0)

/* Columns the cursor advances: everything except \r and complete CSI escapes.
 * Also reports whether any escape was left truncated -- a byte-offset cut would
 * wedge the terminal's colour state and print the escape's tail as text. */
static int visible_width(const std::string &s, bool *escape_intact)
{
	int vis = 0;
	*escape_intact = true;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		if (c == 0x1b) {
			if (i + 1 >= s.size() || s[i + 1] != '[') {
				*escape_intact = false;
				return vis;
			}
			size_t j = i + 2;
			while (j < s.size() && (unsigned char)s[j] >= 0x30 &&
			                       (unsigned char)s[j] <= 0x3f) ++j;
			while (j < s.size() && (unsigned char)s[j] >= 0x20 &&
			                       (unsigned char)s[j] <= 0x2f) ++j;
			if (j >= s.size()) {   /* ran out before the final byte */
				*escape_intact = false;
				return vis;
			}
			i = j + 1;
			continue;
		}
		if (c < 0x20) { ++i; continue; }   /* \r -- zero width */
		++vis;
		++i;
	}
	return vis;
}

struct Case {
	const char *name;
	DlunaStatus st;
};

int main()
{
	/* dluna_console_init() is deliberately NOT called: g_tty/g_vt_ok default
	 * to true, so dluna_clr_eol() is "\033[K" here regardless of how ctest
	 * attaches stdout. The tail assertion uses the same accessor either way. */
	const std::string tail = std::string("\033[0m") + dluna_clr_eol();

	std::vector<Case> cases = {
		/* rate  avg    height     mb      blk     rej   diff     hh mm ss */
		{"zeros",     {0.0,   0.0,  0,         0,      0,      0,    "0",    0,  0,  0}},
		{"screenshot",{0.0,   4.96, 7368356,   962,    93,     25,   "795K", 0,  1, 11}},
		{"typical",   {20.91, 20.71, 7212998,  1998,   262,    0,    "20K",  3, 14,  7}},
		{"maxed",     {9999.99, 9999.99, 999999999LL, 9999999LL, 9999999LL,
		               9999999LL, "999G", 99, 59, 59}},
	};

	const int widths[] = {0, 10, 20, 28, 34, 40, 56, 60, 72, 80, 86, 100,
	                      110, 116, 120, 200};

	int full_width_ref = -1;

	for (const Case &c : cases) {
		for (int cols : widths) {
			char buf[512];
			size_t n = dluna_format_status(buf, sizeof buf, c.st, cols);
			std::string out(buf, n);

			CHECK(n == std::strlen(buf),
			      "[%s cols=%d] returned length %zu != strlen %zu",
			      c.name, cols, n, std::strlen(buf));

			bool intact = false;
			int vis = visible_width(out, &intact);

			CHECK(intact, "[%s cols=%d] truncated ANSI escape in output",
			      c.name, cols);

			if (cols > 0) {
				CHECK(vis <= cols - 1,
				      "[%s cols=%d] visible width %d exceeds budget %d "
				      "-- this line WILL wrap and stack",
				      c.name, cols, vis, cols - 1);
			}

			/* Exactly one leading \r and no newline: a newline would
			 * scroll the row away, defeating in-place repainting. */
			CHECK(out.size() > 0 && out[0] == '\r',
			      "[%s cols=%d] does not start with \\r", c.name, cols);
			CHECK(out.find('\r', 1) == std::string::npos,
			      "[%s cols=%d] contains a second \\r", c.name, cols);
			CHECK(out.find('\n') == std::string::npos,
			      "[%s cols=%d] contains a newline", c.name, cols);

			/* Colour must always be reset and the row cleared, or a
			 * shorter line leaves the previous tick's tail on screen. */
			CHECK(out.size() >= tail.size() &&
			      out.compare(out.size() - tail.size(), tail.size(), tail) == 0,
			      "[%s cols=%d] does not end with reset + erase-to-EOL",
			      c.name, cols);
		}
	}

	/* cols <= 0 means "width unknown" and must reproduce the pre-1.0.26 line
	 * byte for byte, so a failed width query is never a visible regression. */
	{
		DlunaStatus st = cases[1].st;
		char buf[512];
		dluna_format_status(buf, sizeof buf, st, 0);
		char legacy[512];
		std::snprintf(legacy, sizeof legacy,
		              "\r\033[93m[DIRTYBIRD-C] \033[92m%.2f KH/s\033[97m "
		              "(\033[32m%.2f KH/s avg\033[97m) | \033[34mHeight:%lld\033[97m | "
		              "\033[36mMiniblocks:%lld\033[97m | \033[32mBlocks:%lld\033[97m | "
		              "%sREJ:%lld\033[97m | \033[35mDiff:%s\033[97m | "
		              "\033[37m%02d:%02d:%02d\033[0m%s",
		              st.rate, st.avg, st.height, st.accepted, st.blocks,
		              st.rejected > 0 ? "\033[91m" : "\033[37m", st.rejected,
		              st.diff, st.hh, st.mm, st.ss, dluna_clr_eol());
		CHECK(std::strcmp(buf, legacy) == 0,
		      "cols=0 must be byte-identical to the legacy full line\n"
		      "  got:      %s\n  expected: %s", buf, legacy);

		bool intact = false;
		full_width_ref = visible_width(std::string(buf), &intact);
		CHECK(full_width_ref == 117,
		      "screenshot case should be 117 visible columns, got %d",
		      full_width_ref);
	}

	/* A desktop console wide enough for the full layout must still get it. */
	{
		char wide[512], unknown[512];
		dluna_format_status(wide, sizeof wide, cases[1].st, 200);
		dluna_format_status(unknown, sizeof unknown, cases[1].st, 0);
		CHECK(std::strcmp(wide, unknown) == 0,
		      "cols=200 must select the full layout unchanged");
	}

	/* The reported 56-column Termux width: must fit, and must still carry the
	 * counters -- fitting by throwing every field away is not a fix. */
	{
		char buf[512];
		dluna_format_status(buf, sizeof buf, cases[1].st, 56);
		std::string out(buf);
		bool intact = false;
		int vis = visible_width(out, &intact);
		CHECK(vis <= 55, "56-column render is %d columns wide", vis);
		CHECK(out.find("MB:962") != std::string::npos,
		      "56-column render dropped the miniblock counter: %s", buf);
		CHECK(out.find("B:93") != std::string::npos,
		      "56-column render dropped the block counter: %s", buf);
		CHECK(out.find("R:25") != std::string::npos,
		      "56-column render dropped the reject counter: %s", buf);
		std::printf("56-col render (%d cols visible): %s\n",
		            vis, out.c_str() + 1);
	}

	/* Two dluna_term_cols() branches are reachable from ctest, one per test
	 * registration: `statusline_env_cols` sets DIRTYBIRD_C_COLS and pins the
	 * override (the escape hatch for setups where the ioctl reports 0), while
	 * plain `statusline` leaves it unset and lands on the query-failed
	 * fallback, because ctest gives us a pipe rather than a real terminal. */
	if (const char *env = std::getenv("DIRTYBIRD_C_COLS")) {
		int want = std::atoi(env);
		int got  = dluna_term_cols();
		CHECK(got == want, "DIRTYBIRD_C_COLS=%s but dluna_term_cols() == %d",
		      env, got);

		char buf[512];
		dluna_format_status(buf, sizeof buf, cases[1].st, got);
		bool intact = false;
		int vis = visible_width(std::string(buf), &intact);
		CHECK(intact && vis <= got - 1,
		      "override width %d rendered %d columns", got, vis);
		std::printf("statusline: DIRTYBIRD_C_COLS=%d honored (%d columns)\n",
		            got, vis);
	} else {
		/* No override: this is the width-query-failed path. g_tty is still its
		 * default true here (dluna_console_init() is deliberately never called),
		 * and ctest hands us a pipe, so both TIOCGWINSZ and
		 * GetConsoleScreenBufferInfo fail -- exactly the case that used to
		 * answer 0. A 0 here is an INT_MAX width budget in
		 * dluna_format_status(), i.e. the full 115-column line on a phone
		 * terminal, which wraps and then stacks one row per repaint.
		 *
		 * Asserted as "> 0" rather than "== 80" so this still holds when a
		 * developer runs the binary straight from a real terminal, where the
		 * query succeeds and returns that terminal's actual width. */
		int got = dluna_term_cols();
		CHECK(got > 0,
		      "dluna_term_cols() == %d on a console of unknown width -- 0 is an "
		      "unbounded budget and the status line will wrap and stack", got);

		char buf[512];
		dluna_format_status(buf, sizeof buf, cases[1].st, got);
		bool intact = false;
		int vis = visible_width(std::string(buf), &intact);
		CHECK(intact && vis <= got - 1,
		      "fallback width %d rendered %d columns", got, vis);
		std::printf("statusline: unknown-width fallback = %d columns "
		            "(%d rendered)\n", got, vis);
	}

	/* The row is silent until there is something true to report. A 0.00 KH/s
	 * readout is not a rare fault -- it is what every launch printed, because
	 * getwork sends no job at connect time and the first tick lands in that
	 * gap. Reconnect is the same story. */
	{
		/* Every launch: not connected, no job. */
		CHECK(!dluna_status_row_visible(false, 0),
		      "painted a row before connecting");
		/* Connected, but no job pushed yet -- workers are still parked, so
		 * the rate is a genuine zero. */
		CHECK(!dluna_status_row_visible(true, 0),
		      "painted a row before the first job arrived");
		/* Mining. */
		CHECK(dluna_status_row_visible(true, 1),
		      "suppressed the row while actually mining");
		/* Dropped mid-run: a job was seen earlier so jobEpoch stays high,
		 * but nothing can hash until the redial lands. Having once had a
		 * job must not keep the row alive. */
		CHECK(!dluna_status_row_visible(false, 5),
		      "kept the row alive across a disconnect");
		std::printf("statusline: idle-row suppression OK\n");
	}

	if (g_failures == 0) {
		std::printf("statusline: PASS\n");
		return 0;
	}
	std::printf("statusline: %d FAILURE(S)\n", g_failures);
	return 1;
}
