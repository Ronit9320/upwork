/*
 * Chinese Checkers Engine
 */

/*
 * To implement this module, remove the following #if 0 and the matching #endif.
 * Fill in your implementation of the engine() function below (you may also add
 * other functions).  When you compile the program, your implementation will be
 * incorporated.  If you leave the #if 0 here, then your program will be linked
 * with a demonstration version of the engine.  You can use this feature to work
 * on your implementation of the main program before you attempt to implement
 * the engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <time.h>

#include "ccheck.h"
#include "debug.h"
#include <unistd.h>

extern int depth;
extern Move principal_var[];   // PV filled by bestmove()
extern int bestmove(Board *bp, Player p, int ply, Move pv[], int alpha, int beta);

/* Optional but very helpful if your lib exports them (no-op if not): */
extern void reset_stats(void);
extern void print_stats(void);
extern void print_pvar(Board *bp, int ply);
extern void timings(int d);

/* Time-control knobs from the lib (safe defaults below if they’re 0): */
extern int avgtime;


static int read_line(char *buf, size_t n, FILE *in) {
    if (!fgets(buf, (int)n, in))
        return 0; /* EOF or error -> parent will see EOF if we exit */
    return 1;
}

/* Parse ">white:<move>" or ">black:<move>" and return a Move. */
static Move parse_forwarded_move(Board *bp, const char *line) {
    const char *colon = strchr(line, ':');
    if (!colon) return 0;
    const char *mvtxt = colon + 1;

    /* Use a memory FILE to reuse the library parser+legality checker. */
    FILE *mem = fmemopen((void *)mvtxt, strlen(mvtxt), "r");
    if (!mem) return 0;
    Move m = read_move_from_pipe(mem, bp);
    fclose(mem);
    return m; /* 0 only if EOF was in mvtxt (malformed) */
}

void engine(Board *bp) {

fprintf(stderr, "[engine] engine starts\n"); //ming

	/* The parent uses SIGHUP as a "nudge". Default action for SIGHUP is terminate.
       Ignore it so our blocking I/O loop keeps running. */
	signal(SIGHUP, SIG_IGN);

    /* Make stdout line-buffered so each line flushes to the parent immediately. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    char line[256];

    for (;;) {
        /* Block on a line from the parent. Parent also sends SIGHUP, which we ignore. */
        if (!read_line(line, sizeof(line), stdin)) {
            /* Parent closed pipe: exit cleanly. */
            return;
        }

        /* Skip blank lines */
        if (line[0] == '\n' || line[0] == '\0')
            continue;
        fprintf(stderr, "[engine] got control: %s", line);  /* still stderr */

        if (line[0] == '>') {
fprintf(stderr, "[engine] line 87\n"); //ming

			Move m = parse_forwarded_move(bp, line);  /* or your existing wrapper */
		    if (m != 0) apply(bp, m);
		    /* Always ack so parent doesn’t wedge if we were conservative */
		    if (write(STDOUT_FILENO, "ok\n", 3) != 3) {
		        fprintf(stderr, "[engine] write(stdout) ack failed\n");
		    }
		    continue;
//			/* Opponent move forwarded: ">white:<move>" or ">black:<move>" */
//            Move m = parse_forwarded_move(bp, line);
//            if (m != 0) {
//                apply(bp, m);
//                fputs("ok\n", stdout);   /* required ack to parent */
//                //fprintf(stdout, "ok!\n");
//                fflush(stdout);
//            } else {
//                /* Malformed line: be conservative and ack anyway to avoid wedging */
//                fputs("ok\n", stdout);
//                //fprintf(stdout, "ok\n");
//                fflush(stdout);
//            }
//            continue;
        } else if (line[0] == '<') {
            /* Our turn: compute and emit exactly one legal move to parent's stdout pipe */
            fprintf(stderr, "[engine] searching (iterative deepening)...\n");

            Player me = player_to_move(bp);
            (void)bestmove(bp, me, 0, principal_var, -MAXEVAL, +MAXEVAL);
            Move best = principal_var[0];
            if (best == 0) {
                depth = 1;
                (void)bestmove(bp, me, 0, principal_var, -MAXEVAL, +MAXEVAL);
                best = principal_var[0];
                if (best == 0) { fprintf(stderr, "[engine] ERROR: no move\n"); continue; }
            }

            /* Emit EXACTLY ONE line to stdout */
            print_move(bp, best, stdout);   /* prints "<move>" */
            fprintf(stdout, "\n");          /* add the required newline */
            fflush(stdout);                 /* make sure it leaves the pipe now */

            apply(bp, best);
            fprintf(stderr, "[engine] played.\n");
            continue;

//11-10
//        	    /* Our turn: compute and emit exactly one legal move to parent's stdout pipe */
//			    fprintf(stderr, "[engine] searching (iterative deepening)...\n");
//
//				Player me = player_to_move(bp);
//			    (void)bestmove(bp, me, 0, principal_var, -MAXEVAL, +MAXEVAL);
//			    Move best = principal_var[0];
//			    if (best == 0) {
//			        depth = 1;
//			        (void)bestmove(bp, me, 0, principal_var, -MAXEVAL, +MAXEVAL);
//			        best = principal_var[0];
//			        if (best == 0) { fprintf(stderr, "[engine] ERROR: no move\n"); continue; }
//			    }
//char mvbuf[128];
//FILE *mem = fmemopen(mvbuf, sizeof mvbuf, "w");
//print_move(bp, best, mem);
//fflush(mem);
//fclose(mem);
//
//fputs(mvbuf, stdout);  fflush(stdout);  //ming
//fprintf(stderr, "[engine mirror] %s\n", mvbuf);  //ming
//
//			    /* Emit EXACT canonical text that the library prints */
//			    print_move(bp, best, stdout);   /* this includes the trailing '\n' */
//			    fflush(stdout);
//
//			    apply(bp, best);
//			    fprintf(stderr, "[engine] played.\n");
//			    continue;


//11-09
// 			/* Our turn: iterative deepening using the lib's global principal_var[] */
//            fprintf(stderr, "[engine] searching (iterative deepening)...\n");
// 			Player me = player_to_move(bp);
//
//    		// run your search
//    		(void)bestmove(bp, me, /*ply*/0, principal_var, -MAXEVAL, +MAXEVAL);
//    		Move best = principal_var[0];
//
//		    // minimal fallback if search returned empty PV
//		    if (best == 0) {
//
//		        depth = 1;
//		        (void)bestmove(bp, me, 0, principal_var, -MAXEVAL, +MAXEVAL);
//		        best = principal_var[0];
//		    }
//
//		    if (best == 0) {
//		        fprintf(stderr, "[engine] ERROR: no move to emit.\n");
//		        continue;  // parent will block, but we at least log the reason
//		    }
//
////fprintf(stderr, "[engine] 11\n"); //ming
//
//		    // **** THE CRITICAL PART ****
//		    print_move(bp, best, stdout);  // writes "<move>\n" to stdout
//
//
//// Prepare a printable version of the move
//char mvbuf[128];
//FILE *mem = fmemopen(mvbuf, sizeof mvbuf, "w");
//print_move(bp, best, mem);
//fflush(mem);
//fclose(mem);
//
//// Send move to parent (stdout)
//fputs(mvbuf, stdout);
//
//
//// ALSO mirror to stderr for debugging
//fprintf(stderr, "[engine mirror] %s\n", mvbuf);
//
//
//		    fflush(stdout);                // ensure it leaves the pipe immediately
//		    apply(bp, best);               // keep engine board in sync
//
//		    fprintf(stderr, "[engine] played.\n"); // keep this on stderr only
//		    continue;
        } else {
            /* Unknown control line: ignore. */
            continue;
        }
    }
}
